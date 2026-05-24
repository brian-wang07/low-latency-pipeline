#include "shm_segment.hpp"

#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

bool ShmSegment::create(const char *label, std::size_t size) {
  if (is_valid_)
    return false;

  fd_ = ::memfd_create(label, MFD_HUGETLB);
  if (fd_ == -1) {
    std::perror("memfd_create failed");
    return false;
  }

  if (::ftruncate(fd_, static_cast<off_t>(size)) == -1) {
    std::perror("ftruncate failed");
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  addr_ = ::mmap(nullptr, size, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_POPULATE, fd_, 0);
  if (addr_ == MAP_FAILED) {
    std::perror("mmap failed");
    ::close(fd_);
    fd_ = -1;
    addr_ = nullptr;
    return false;
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
