#include "shm_segment.hpp"

#include "common/ipc/layout.hpp"
#include "common/platform/spin_pause.hpp"
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>

// Maps a fresh memfd of `size`; returns false (fd closed) on any failure.
static bool map_memfd(const char *label, std::size_t size, unsigned flags,
                      int &fd, void *&addr) {
  fd = ::memfd_create(label, flags);
  if (fd == -1)
    return false;
  if (::ftruncate(fd, static_cast<off_t>(size)) == -1) {
    ::close(fd);
    fd = -1;
    return false;
  }
  addr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_POPULATE, fd, 0);
  if (addr == MAP_FAILED) {
    ::close(fd);
    fd = -1;
    addr = nullptr;
    return false;
  }
  return true;
}

bool ShmSegment::create(const char *label, std::size_t size) {
  if (is_valid_)
    return false;

  // An empty hugepage pool surfaces as mmap ENOMEM under MAP_POPULATE, not as a
  // memfd_create error, so fall back on either.
  if (!map_memfd(label, size, MFD_HUGETLB, fd_, addr_)) {
    std::perror("hugetlb shm unavailable");
    std::fprintf(stderr, "warning: falling back to normal pages for shm\n");
    if (!map_memfd(label, size, 0, fd_, addr_)) {
      std::perror("shm create failed");
      return false;
    }
  }

  size_ = size;
  owner_ = true;
  is_valid_ = true;
  return true;
}

bool ShmSegment::attach(int fd, std::size_t size) {
  if (is_valid_ || fd < 0)
    return false;

  addr_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, fd, 0);
  if (addr_ == MAP_FAILED) {
    std::perror("attach mmap failed");
    addr_ = nullptr;
    return false;
  }

  fd_ = fd;
  size_ = size;
  owner_ = false;
  is_valid_ = true;
  return true;
}

void ShmSegment::reset_() noexcept {
  if (addr_ && addr_ != MAP_FAILED)
    ::munmap(addr_, size_);
  if (fd_ != -1)
    ::close(fd_);
  addr_ = nullptr;
  fd_ = -1;
  size_ = 0;
  owner_ = false;
  is_valid_ = false;
}

ShmSegment::~ShmSegment() noexcept { reset_(); }

ShmSegment::ShmSegment(ShmSegment &&other) noexcept
    : fd_(other.fd_), addr_(other.addr_), size_(other.size_),
      owner_(other.owner_), is_valid_(other.is_valid_) {
  other.fd_ = -1;
  other.addr_ = nullptr;
  other.size_ = 0;
  other.owner_ = false;
  other.is_valid_ = false;
}

ShmSegment &ShmSegment::operator=(ShmSegment &&other) noexcept {
  if (this != &other) {
    reset_();
    fd_ = other.fd_;
    addr_ = other.addr_;
    size_ = other.size_;
    owner_ = other.owner_;
    is_valid_ = other.is_valid_;
    other.fd_ = -1;
    other.addr_ = nullptr;
    other.size_ = 0;
    other.owner_ = false;
    other.is_valid_ = false;
  }
  return *this;
}

ipc::PipelineShm *ipc::attach_pipeline(const char *fd_arg, ShmSegment &shm,
                                       const char *who) noexcept {
  char *end = nullptr;
  const long fd = fd_arg ? std::strtol(fd_arg, &end, 10) : -1;
  if (!fd_arg || end == fd_arg || *end != '\0' || fd < 0 || fd > INT_MAX) {
    std::fprintf(stderr, "%s: bad shm fd argument '%s'\n", who,
                 fd_arg ? fd_arg : "(null)");
    std::exit(2);
  }
  if (!shm.attach(static_cast<int>(fd), SHM_SIZE)) {
    std::fprintf(stderr, "%s: cannot map shm fd %ld\n", who, fd);
    std::exit(2);
  }
  auto *p = shm.as<PipelineShm>();
  while (p->header.magic.load(std::memory_order_acquire) == 0)
    SPIN_PAUSE();
  if (p->header.magic.load(std::memory_order_relaxed) != MAGIC ||
      p->header.version != VERSION || p->header.layout_hash != LAYOUT_HASH) {
    std::fprintf(stderr,
                 "%s: shm layout mismatch: segment magic %llx version %u "
                 "layout %08x, this binary expects magic %llx version %u "
                 "layout %08x; rebuild all binaries\n",
                 who,
                 (unsigned long long)p->header.magic.load(std::memory_order_relaxed),
                 p->header.version, p->header.layout_hash,
                 (unsigned long long)MAGIC, VERSION, LAYOUT_HASH);
    std::exit(2);
  }
  return p;
}
