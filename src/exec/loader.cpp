#include "exec/loader.hpp"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace exec {

namespace {

bool fail(char *err, size_t n, const char *fmt, const char *a = "",
          const char *b = "") noexcept {
  std::snprintf(err, n, fmt, a, b);
  return false;
}

bool is_module_path(const char *spec) noexcept {
  const size_t n = std::strlen(spec);
  return std::strchr(spec, '/') != nullptr ||
         (n > 3 && std::strcmp(spec + n - 3, ".so") == 0);
}

// The module's ELF program headers must not include PT_TLS: thread_local in a
// module means dynamic TLS (__tls_get_addr) on the hot path.
bool check_elf(const unsigned char *img, size_t len, char *err,
               size_t n) noexcept {
  if (len < sizeof(Elf64_Ehdr) || std::memcmp(img, ELFMAG, SELFMAG) != 0)
    return fail(err, n, "not an ELF file");
  Elf64_Ehdr eh;
  std::memcpy(&eh, img, sizeof(eh));
  if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_machine != EM_X86_64 ||
      eh.e_type != ET_DYN)
    return fail(err, n, "not an x86-64 shared object");
  if (eh.e_phoff + size_t(eh.e_phnum) * sizeof(Elf64_Phdr) > len)
    return fail(err, n, "truncated program headers");
  for (int i = 0; i < eh.e_phnum; ++i) {
    Elf64_Phdr ph;
    std::memcpy(&ph, img + eh.e_phoff + size_t(i) * sizeof(ph), sizeof(ph));
    if (ph.p_type == PT_TLS)
      return fail(err, n, "module has thread-local storage (PT_TLS)");
  }
  return true;
}

struct FaultArgs {
  const char *path;
  bool found;
};

// Touches every page of the module's loaded segments and locks them, so the first
// ticks after a load don't take page faults. mlock is best effort.
int prefault(dl_phdr_info *info, size_t, void *data) {
  auto *a = static_cast<FaultArgs *>(data);
  if (!info->dlpi_name || std::strcmp(info->dlpi_name, a->path) != 0)
    return 0;
  a->found = true;
  const long page = sysconf(_SC_PAGESIZE);
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) &ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD)
      continue;
    const uintptr_t start = (info->dlpi_addr + ph.p_vaddr) & ~uintptr_t(page - 1);
    const uintptr_t end = info->dlpi_addr + ph.p_vaddr + ph.p_memsz;
    for (uintptr_t p = start; p < end; p += uintptr_t(page))
      (void)*reinterpret_cast<volatile const char *>(p);
    (void)mlock(reinterpret_cast<void *>(start), end - start);
  }
  return 1;
}

bool check_module(const ll_strategy_module_v1 *m, char *err, size_t n) noexcept {
  if (m->abi_version != MODULE_ABI_VERSION)
    return fail(err, n, "module ABI version mismatch");
  if (m->layout_hash != exec_abi_hash())
    return fail(err, n,
                "module '%s' was built against a different shm/exec layout "
                "(build %s); rebuild it",
                m->name, m->build_id);
  if (m->isa_mask & ~cpu_isa_mask())
    return fail(err, n, "module '%s' uses CPU features this machine lacks (built "
                        "with -march=native elsewhere?)",
                m->name);
  if (!m->create || !m->run_null || !m->destroy)
    return fail(err, n, "module '%s' is missing entry points", m->name);
  return true;
}

} // namespace

bool load_strategy(const char *spec, LoadedModule &out, char *err,
                   size_t errlen) noexcept {
  out = {};
  if (!is_module_path(spec)) {
    const char *name = std::strncmp(spec, "builtin:", 8) == 0 ? spec + 8 : spec;
    out.m = module_detail::Builtins::find(name);
    if (!out.m)
      return fail(err, errlen, "no builtin strategy '%s' in this binary", name);
    return check_module(out.m, err, errlen);
  }

  const int fd = ::open(spec, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return fail(err, errlen, "cannot open module %s", spec);
  struct stat st{};
  if (::fstat(fd, &st) != 0 || st.st_size <= 0) {
    ::close(fd);
    return fail(err, errlen, "cannot stat module %s", spec);
  }
  const size_t len = size_t(st.st_size);
  void *img = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (img == MAP_FAILED)
    return fail(err, errlen, "cannot read module %s", spec);

  bool ok = check_elf(static_cast<const unsigned char *>(img), len, err, errlen);
  int mfd = -1;
  if (ok) {
    mfd = ::memfd_create("strategy_module", MFD_CLOEXEC);
    size_t off = 0;
    while (mfd >= 0 && off < len) {
      const ssize_t w = ::write(mfd, static_cast<const char *>(img) + off, len - off);
      if (w <= 0)
        break;
      off += size_t(w);
    }
    ok = mfd >= 0 && off == len;
    if (!ok)
      fail(err, errlen, "cannot copy module %s into a memfd", spec);
  }
  ::munmap(img, len);
  if (!ok) {
    if (mfd >= 0)
      ::close(mfd);
    return false;
  }

  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/fd/%d", mfd);
  out.handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!out.handle) {
    ::close(mfd);
    return fail(err, errlen, "dlopen failed: %s", dlerror());
  }
  using EntryFn = const ll_strategy_module_v1 *(*)();
  auto entry = reinterpret_cast<EntryFn>(::dlsym(out.handle, "ll_strategy_module"));
  if (!entry || !(out.m = entry())) {
    ::dlclose(out.handle);
    ::close(mfd);
    out = {};
    return fail(err, errlen, "%s exports no ll_strategy_module", spec);
  }
  if (!check_module(out.m, err, errlen)) {
    ::dlclose(out.handle);
    ::close(mfd);
    out = {};
    return false;
  }
  FaultArgs fa{path, false};
  dl_iterate_phdr(prefault, &fa);
  out.memfd = mfd;
  return true;
}

} // namespace exec
