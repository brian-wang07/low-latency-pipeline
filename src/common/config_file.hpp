#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace common {

// key=value config (.cfg). One entry per line, '#' starts a full-line comment,
// whitespace around keys and values is trimmed, and a repeated key overrides the
// earlier one. Fixed storage, no allocation, no exceptions: problems are printed
// and the getters fall back to their defaults.
class Config {
public:
  static constexpr int MAX_ENTRIES = 256;
  static constexpr int KEY_LEN = 64;
  static constexpr int VAL_LEN = 256;

  // Returns false if the file can't be read or any line was rejected.
  bool load(const char *path) noexcept {
    std::FILE *f = std::fopen(path, "r");
    if (!f) {
      std::fprintf(stderr, "config: cannot open %s\n", path);
      return false;
    }
    source_ = path;
    bool ok = true;
    char line[VAL_LEN + KEY_LEN + 64];
    int lineno = 0;
    while (std::fgets(line, sizeof(line), f)) {
      ++lineno;
      const std::size_t len = std::strlen(line);
      if (len == sizeof(line) - 1 && line[len - 1] != '\n') {
        warn(lineno, "line too long");
        ok = false;
        int c;
        while ((c = std::fgetc(f)) != '\n' && c != EOF) {
        }
        continue;
      }
      ok &= parse_line(line, lineno);
    }
    std::fclose(f);
    return ok;
  }

  // Parses newline-separated text (tests, inline defaults).
  bool parse(const char *text) noexcept {
    source_ = "<text>";
    bool ok = true;
    int lineno = 0;
    char line[VAL_LEN + KEY_LEN + 64];
    while (*text) {
      ++lineno;
      const char *nl = std::strchr(text, '\n');
      std::size_t n = nl ? std::size_t(nl - text) : std::strlen(text);
      if (n >= sizeof(line)) {
        warn(lineno, "line too long");
        ok = false;
      } else {
        std::memcpy(line, text, n);
        line[n] = '\0';
        ok &= parse_line(line, lineno);
      }
      text += n + (nl ? 1 : 0);
    }
    return ok;
  }

  // Workers take "--config=<path>" anywhere after their positional arguments.
  // Loads it if present; returns false only if one was given and failed to load.
  bool load_from_args(int argc, char **argv) noexcept {
    for (int i = 1; i < argc; ++i)
      if (std::strncmp(argv[i], "--config=", 9) == 0)
        return load(argv[i] + 9);
    return true;
  }

  bool has(const char *key) const noexcept { return find(key) != nullptr; }
  int size() const noexcept { return n_; }

  const char *get_str(const char *key, const char *def) const noexcept {
    const Entry *e = find(key);
    return e ? e->val : def;
  }

  // Accepts digit-group underscores (20_000_000).
  int64_t get_i64(const char *key, int64_t def) const noexcept {
    const Entry *e = find(key);
    if (!e)
      return def;
    char buf[VAL_LEN];
    int j = 0;
    for (const char *p = e->val; *p; ++p)
      if (*p != '_')
        buf[j++] = *p;
    buf[j] = '\0';
    char *end = nullptr;
    const long long v = std::strtoll(buf, &end, 10);
    if (end == buf || *end != '\0') {
      std::fprintf(stderr, "config: %s=%s is not an integer, using %lld\n", key,
                   e->val, (long long)def);
      return def;
    }
    return v;
  }

  double get_f64(const char *key, double def) const noexcept {
    const Entry *e = find(key);
    if (!e)
      return def;
    char *end = nullptr;
    const double v = std::strtod(e->val, &end);
    if (end == e->val || *end != '\0') {
      std::fprintf(stderr, "config: %s=%s is not a number, using %g\n", key,
                   e->val, def);
      return def;
    }
    return v;
  }

  bool get_bool(const char *key, bool def) const noexcept {
    const Entry *e = find(key);
    if (!e)
      return def;
    const char *v = e->val;
    if (!std::strcmp(v, "1") || !std::strcmp(v, "true") || !std::strcmp(v, "yes"))
      return true;
    if (!std::strcmp(v, "0") || !std::strcmp(v, "false") || !std::strcmp(v, "no"))
      return false;
    std::fprintf(stderr, "config: %s=%s is not a bool, using %d\n", key, v, def);
    return def;
  }

  // Calls f(suffix, value) for every key starting with prefix, in file order.
  template <class F> void for_prefix(const char *prefix, F &&f) const noexcept {
    const std::size_t pl = std::strlen(prefix);
    for (int i = 0; i < n_; ++i)
      if (std::strncmp(e_[i].key, prefix, pl) == 0)
        f(e_[i].key + pl, e_[i].val);
  }

  // Prints keys matching none of `known`; an entry ending in '.' matches a prefix.
  // Returns the number of unknown keys.
  int warn_unknown(const char *const *known, int n_known) const noexcept {
    int unknown = 0;
    for (int i = 0; i < n_; ++i) {
      bool match = false;
      for (int k = 0; k < n_known && !match; ++k) {
        const std::size_t kl = std::strlen(known[k]);
        match = (kl > 0 && known[k][kl - 1] == '.')
                    ? std::strncmp(e_[i].key, known[k], kl) == 0
                    : std::strcmp(e_[i].key, known[k]) == 0;
      }
      if (!match) {
        std::fprintf(stderr, "config: %s: unknown key '%s'\n", source_,
                     e_[i].key);
        ++unknown;
      }
    }
    return unknown;
  }

private:
  struct Entry {
    char key[KEY_LEN];
    char val[VAL_LEN];
  };

  static char *trim(char *s) noexcept {
    while (*s == ' ' || *s == '\t')
      ++s;
    char *end = s + std::strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\n' ||
                       end[-1] == '\r'))
      *--end = '\0';
    return s;
  }

  void warn(int lineno, const char *what) const noexcept {
    std::fprintf(stderr, "config: %s:%d: %s\n", source_, lineno, what);
  }

  bool parse_line(char *line, int lineno) noexcept {
    char *s = trim(line);
    if (*s == '\0' || *s == '#')
      return true;
    char *eq = std::strchr(s, '=');
    if (!eq) {
      warn(lineno, "expected key=value");
      return false;
    }
    *eq = '\0';
    char *key = trim(s);
    char *val = trim(eq + 1);
    if (*key == '\0') {
      warn(lineno, "empty key");
      return false;
    }
    if (std::strlen(key) >= KEY_LEN || std::strlen(val) >= VAL_LEN) {
      warn(lineno, "key or value too long");
      return false;
    }
    Entry *e = const_cast<Entry *>(find(key));
    if (!e) {
      if (n_ == MAX_ENTRIES) {
        warn(lineno, "too many entries");
        return false;
      }
      e = &e_[n_++];
      std::strcpy(e->key, key);
    }
    std::strcpy(e->val, val);
    return true;
  }

  const Entry *find(const char *key) const noexcept {
    for (int i = 0; i < n_; ++i)
      if (std::strcmp(e_[i].key, key) == 0)
        return &e_[i];
    return nullptr;
  }

  Entry e_[MAX_ENTRIES];
  int n_ = 0;
  const char *source_ = "<none>";
};

} // namespace common
