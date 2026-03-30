// Copyright 2021 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "spraypaint.h"

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>

#include "absl/flags/flag.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "log.h"
#include "two_color.h"

#ifndef TEMP_FAILURE_RETRY
// Musl doesn't provide TEMP_FAILURE_RETRY; emulate it without GNU extensions.
#define TEMP_FAILURE_RETRY(expr) \
  ([&]() {                        \
    auto _rc = (expr);            \
    while (_rc == -1 && errno == EINTR) { \
      _rc = (expr);               \
    }                             \
    return _rc;                   \
  }())
#endif

#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif

ABSL_FLAG(bool, ignore_affinity_failure, false,
          "Silently ignore affinity failure");

namespace gvisor {
namespace {
const size_t kPageSize = sysconf(_SC_PAGESIZE);
constexpr size_t kHugePageSize = 2 * 1024 * 1024;
constexpr size_t kMaxZeroCopyChunk = 64 * 1024;
std::atomic<uint64_t> g_cache_sink(0);

size_t RoundUpToPageSize(size_t k) {
  return ((k + kPageSize - 1) / kPageSize) * kPageSize;
}

uint8_t RotateLeft(uint8_t value) {
  return static_cast<uint8_t>((value << 1) | (value >> 7));
}

uint8_t StressByte(uint8_t value, uint8_t salt, size_t position) {
  const uint8_t lane =
      static_cast<uint8_t>(((position * 17u) + (position >> 2)) & 0xffu);
  return RotateLeft(static_cast<uint8_t>(value + salt + lane));
}

void PrefetchRead(const uint8_t *ptr) {
#if defined(__clang__) || defined(__GNUC__)
  __builtin_prefetch(ptr, 0, 3);
#else
  (void)ptr;
#endif
}

void PrefetchWrite(const uint8_t *ptr) {
#if defined(__clang__) || defined(__GNUC__)
  __builtin_prefetch(ptr, 1, 3);
#else
  (void)ptr;
#endif
}

class ScopedFd {
 public:
  ScopedFd() = default;
  explicit ScopedFd(int fd) : fd_(fd) {}
  ~ScopedFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  int release() {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int fd = -1) {
    if (fd_ >= 0) {
      close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

class Mapping {
 public:
  Mapping() = default;
  Mapping(uint8_t* data, size_t size) : data_(data), size_(size) {}
  ~Mapping() { reset(); }

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;

  Mapping(Mapping&& other) noexcept
      : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
  }

  Mapping& operator=(Mapping&& other) noexcept {
    if (this != &other) {
      reset();
      data_ = other.data_;
      size_ = other.size_;
      other.data_ = nullptr;
      other.size_ = 0;
    }
    return *this;
  }

  static Mapping Anonymous(size_t size, int prot = PROT_READ | PROT_WRITE,
                           int flags = MAP_ANONYMOUS | MAP_PRIVATE) {
    void* p = mmap(nullptr, size, prot, flags, -1, 0);
    if (p == MAP_FAILED) {
      return {};
    }
    return Mapping(static_cast<uint8_t*>(p), size);
  }

  static Mapping File(int fd, size_t size, int prot, int flags) {
    void* p = mmap(nullptr, size, prot, flags, fd, 0);
    if (p == MAP_FAILED) {
      return {};
    }
    return Mapping(static_cast<uint8_t*>(p), size);
  }

  uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool valid() const { return data_ != nullptr; }

  void assign(uint8_t* data, size_t size) {
    reset();
    data_ = data;
    size_ = size;
  }

  void reset() {
    if (data_ != nullptr) {
      munmap(data_, size_);
      data_ = nullptr;
      size_ = 0;
    }
  }

 private:
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

bool IsOptionalKernelPathError(int error) {
  return error == ENOSYS || error == EINVAL || error == EPERM ||
         error == ENOTSUP || error == EOPNOTSUPP;
}

bool WriteExact(int fd, const void* buffer, size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(buffer);
  size_t pos = 0;
  while (pos < length) {
    const int rc =
        TEMP_FAILURE_RETRY(write(fd, bytes + pos, length - pos));
    if (rc <= 0) {
      return false;
    }
    pos += static_cast<size_t>(rc);
  }
  return true;
}

bool ReadExact(int fd, void* buffer, size_t length) {
  auto* bytes = static_cast<uint8_t*>(buffer);
  size_t pos = 0;
  while (pos < length) {
    const int rc = TEMP_FAILURE_RETRY(read(fd, bytes + pos, length - pos));
    if (rc <= 0) {
      return false;
    }
    pos += static_cast<size_t>(rc);
  }
  return true;
}

ssize_t SysMemfdCreate(const char* name, unsigned int flags) {
#ifdef SYS_memfd_create
  return syscall(SYS_memfd_create, name, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

ssize_t SysProcessVmWritev(pid_t pid, const struct iovec* local,
                           unsigned long local_count,
                           const struct iovec* remote,
                           unsigned long remote_count, unsigned long flags) {
#ifdef SYS_process_vm_writev
  return syscall(SYS_process_vm_writev, pid, local, local_count, remote,
                 remote_count, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

ssize_t SysProcessVmReadv(pid_t pid, const struct iovec* local,
                          unsigned long local_count,
                          const struct iovec* remote,
                          unsigned long remote_count, unsigned long flags) {
#ifdef SYS_process_vm_readv
  return syscall(SYS_process_vm_readv, pid, local, local_count, remote,
                 remote_count, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

ssize_t SysVmsplice(int fd, const struct iovec* iov, unsigned long nr_segs,
                    unsigned int flags) {
#ifdef SYS_vmsplice
  return syscall(SYS_vmsplice, fd, iov, nr_segs, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

ssize_t SysSplice(int fd_in, off_t* off_in, int fd_out, off_t* off_out,
                  size_t len, unsigned int flags) {
#ifdef SYS_splice
  return syscall(SYS_splice, fd_in, off_in, fd_out, off_out, len, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

ssize_t SysTee(int fd_in, int fd_out, size_t len, unsigned int flags) {
#ifdef SYS_tee
  return syscall(SYS_tee, fd_in, fd_out, len, flags);
#else
  errno = ENOSYS;
  return -1;
#endif
}

void* SysMremap(void* old_address, size_t old_size, size_t new_size,
                unsigned long flags) {
#ifdef SYS_mremap
  return reinterpret_cast<void*>(
      syscall(SYS_mremap, old_address, old_size, new_size, flags));
#else
  errno = ENOSYS;
  return MAP_FAILED;
#endif
}

bool AdviceIfSupported(uint8_t* buffer, size_t buffer_size, int advice,
                       const char* advice_name) {
  if (madvise(buffer, buffer_size, advice) == 0) {
    return true;
  }
  if (IsOptionalKernelPathError(errno)) {
    SAFELOG(WARN) << advice_name << " unavailable: " << strerror(errno);
    return false;
  }
  SAFELOG(ERROR) << advice_name << " failed: " << strerror(errno);
  return false;
}

}  // namespace

const size_t SprayPaint::kMappedBufferSize = 3 * kPageSize;

// Returns true if successful.
bool SprayPaint::TrySetAffinity(int lpu) {
  cpu_set_t cset;
  CPU_ZERO(&cset);
  CPU_SET(lpu, &cset);
  const int rc = sched_setaffinity(0, sizeof(cset), &cset);
  if (rc) {
    if (!absl::GetFlag(FLAGS_ignore_affinity_failure)) {
      SAFELOG(ERROR) << "setaffinity to LPU: " << lpu
                     << " failed: " << strerror(errno);
    }
    return false;
  }
  return true;
}

SprayPaint::SprayPaint(size_t buffer_size, StressPlan stress_plan)
    : buffer_size_(std::max<size_t>(buffer_size, kMappedBufferSize)),
      stress_plan_(stress_plan),
      buffer_(static_cast<uint8_t *>(
          aligned_alloc(kPageSize, RoundUpToPageSize(buffer_size_)))),
      load_store_source_(stress_plan_.load_store_passes == 0
                             ? 0
                             : RoundUpToPageSize(stress_plan_.load_store_bytes)),
      load_store_target_(load_store_source_.size()) {
  if (buffer_ == nullptr) {
    SAFELOG(FATAL) << "Failed to allocate primary buffer";
  }
  SetKid(0);
  Paint();
  CacheHotlineBuffer(buffer_, buffer_size_);
  for (int k = 0; k < 3; k++) {
    if (!ColorIsRight("Ctor")) {
      SAFELOG(FATAL) << "Failed to color papa buffer right";
    }
  }
}

SprayPaint::~SprayPaint() {
  if (buffer_) {
    free(buffer_);
  }
}

void SprayPaint::SetKid(int kid) { kid_ = kid; }

size_t SprayPaint::CurrentIdentity() const {
  return EncodeIdentity(static_cast<size_t>(last_painted_by_), current_epoch_);
}

size_t SprayPaint::EpochForRound(int round, size_t salt) const {
  if (!stress_plan_.epoch_coloring || stress_plan_.epoch_modulus <= 1) {
    return 0;
  }
  const size_t safe_round = round < 0 ? 0 : static_cast<size_t>(round);
  return (safe_round + salt) % stress_plan_.epoch_modulus;
}

size_t SprayPaint::EncodeIdentity(size_t owner, size_t epoch) const {
  if (!stress_plan_.epoch_coloring || stress_plan_.epoch_modulus <= 1 ||
      stress_plan_.epoch_stride == 0) {
    return owner;
  }
  return (owner % stress_plan_.epoch_stride) +
         ((epoch % stress_plan_.epoch_modulus) * stress_plan_.epoch_stride);
}

void SprayPaint::CowPoke() {
  for (size_t k = 0; k < buffer_size_; k += kPageSize) {
    const size_t page = k / kPageSize;
    const size_t offset =
        (page * 131 + stress_plan_.cache_line_size) % kPageSize;
    const size_t kk = k + offset;
    buffer_[kk] = TwoColor::Color(CurrentIdentity(), 0, kk);
  }
}

void SprayPaint::PaintPattern(size_t identity, size_t buffer_id, uint8_t *buffer,
                              size_t buffer_size,
                              size_t base_position) const {
  for (size_t k = 0; k < buffer_size; ++k) {
    buffer[k] = TwoColor::Color(identity, buffer_id, base_position + k);
  }
}

void SprayPaint::Paint() {
  PaintPattern(CurrentIdentity(), 0, buffer_, buffer_size_);
}

bool SprayPaint::ColorIsRight(const std::string &ident) const {
  return ColorIsRight(0, buffer_, buffer_size_, ident);
}

bool SprayPaint::ColorIsRight(size_t buffer_id, const uint8_t *buffer,
                              size_t buffer_size,
                              const std::string &ident) const {
  return PatternIsRight(CurrentIdentity(), buffer_id, buffer, buffer_size,
                        ident);
}

bool SprayPaint::PatternIsRight(size_t identity, size_t buffer_id,
                                const uint8_t *buffer, size_t buffer_size,
                                const std::string &ident,
                                size_t base_position) const {
  bool ok = true;
  Summarizer summarizer(Ident(ident, buffer_id), this, buffer);
  for (size_t k = 0; k < buffer_size; k++) {
    const uint8_t color = buffer[k];
    if (color != TwoColor::Color(identity, buffer_id, base_position + k)) {
      summarizer.Report(k, color, ErrorMessage(color, base_position + k));
      ok = false;
    }
  }
  summarizer.Finish();
  return ok;
}

bool SprayPaint::BufferIsZeroFilled(const uint8_t *buffer, size_t buffer_size,
                                    const std::string &ident) const {
  bool ok = true;
  Summarizer summarizer(Ident(ident, 0), this, buffer);
  for (size_t k = 0; k < buffer_size; ++k) {
    if (buffer[k] != 0) {
      summarizer.Report(
          k, buffer[k],
          absl::StrFormat("ExpectedZero: saw=%d Position: %d",
                          static_cast<int>(buffer[k]), k));
      ok = false;
    }
  }
  summarizer.Finish();
  return ok;
}

void SprayPaint::Writer(int round, int fd) const {
  std::seed_seq seed{static_cast<uint64_t>(kid_), static_cast<uint64_t>(round)};
  std::knuth_b rng(seed);
  size_t p = 0;
  while (p < buffer_size_) {
    size_t remaining = buffer_size_ - p;
    size_t longest = std::min<size_t>(remaining, kMaxTransfer);
    size_t len = std::uniform_int_distribution<int>(1, longest)(rng);
    int rc = TEMP_FAILURE_RETRY(write(fd, buffer_ + p, len));
    if (rc < 0) {
      SAFELOG(FATAL) << "Kid: " << kid_ << " write failed: " << strerror(errno);
    }
    p += rc;
  }
  close(fd);
}

bool SprayPaint::Reader(int round, int fd) const {
  std::seed_seq seed{static_cast<uint64_t>(round), static_cast<uint64_t>(kid_)};
  std::knuth_b rng(seed);
  size_t p = 0;
  uint8_t v[kMaxTransfer];
  int64_t failures = 0;
  constexpr int kSpewLimit = 500;
  while (p < buffer_size_) {
    size_t remaining = std::min<size_t>(sizeof(v), buffer_size_ - p);
    size_t len = std::uniform_int_distribution<int>(1, remaining)(rng);
    int rc = TEMP_FAILURE_RETRY(read(fd, v, len));
    if (rc < 0) {
      SAFELOG(FATAL) << "Kid: " << kid_ << " read failed: " << strerror(errno);
    }
    if (rc == 0) {
      SAFELOG(ERROR) << "Kid: " << kid_ << " unexpected EOF after " << p
                     << " of " << buffer_size_ << " bytes";
      close(fd);
      return true;
    }
    const size_t bytes_read = static_cast<size_t>(rc);
    Summarizer summarizer(Ident("Pipe", 0), this, v);
    for (size_t k = 0; k < bytes_read; k++) {
      if (v[k] != buffer_[p]) {
        failures += 1;
        if (failures < kSpewLimit) {
          summarizer.Report(k, v[k], ErrorMessage(v[k], k));
        }
      }
      p++;
    }
    summarizer.Finish();
  }
  close(fd);
  if (failures > 0) {
    SAFELOG(ERROR) << "Total Pipe failures: " << failures;
  }
  return failures > 0;
}

uint8_t *SprayPaint::MappedBuffer(size_t id) const {
  void *p = mmap(nullptr, kMappedBufferSize, PROT_READ | PROT_WRITE,
                 MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (p == MAP_FAILED) {
    SAFELOG(ERROR) << "Map length: " << kMappedBufferSize
                   << " failed for kid: " << kid_ << " " << strerror(errno);
    return nullptr;
  }
  uint8_t *buffer = static_cast<uint8_t *>(p);
  bool dirty = false;
  Summarizer summarizer(Ident("Mapped", id), this, buffer);
  for (size_t k = 0; k < kMappedBufferSize; k++) {
    const uint8_t color = buffer[k];
    if (color) {
      dirty = true;
      summarizer.Report(
          k, color,
          absl::StrFormat("ExpectedZero: saw=%d Position: %d",
                          static_cast<int>(color), k));
    }
  }
  summarizer.Finish();
  if (dirty) {
    SAFELOG(ERROR) << "Dirty map for kid: " << kid_;
    munmap(buffer, kMappedBufferSize);
    return nullptr;
  }
  PaintPattern(CurrentIdentity(), id, buffer, kMappedBufferSize);
  if (mprotect(buffer, kMappedBufferSize, PROT_READ | PROT_WRITE) != 0) {
    SAFELOG(ERROR) << "mprotect: " << strerror(errno);
    munmap(buffer, kMappedBufferSize);
    return nullptr;
  }
  CacheHotlineBuffer(buffer, kMappedBufferSize);
  return buffer;
}

void SprayPaint::CacheHotlineBuffer(const uint8_t *buffer,
                                    size_t buffer_size) const {
  if (!stress_plan_.cache_hotline || buffer == nullptr || buffer_size == 0) {
    return;
  }

  const size_t line_size = std::max<size_t>(1, stress_plan_.cache_line_size);
  uint64_t sink = 0;
  for (size_t pass = 0; pass < stress_plan_.cache_hotline_passes; ++pass) {
    for (size_t pos = 0; pos < buffer_size; pos += line_size) {
      const size_t next = std::min(buffer_size - 1, pos + line_size);
      PrefetchRead(buffer + next);
      sink += buffer[(pos + pass) % buffer_size];
    }
    size_t tail = buffer_size;
    while (tail > 0) {
      tail = tail > line_size ? tail - line_size : 0;
      PrefetchRead(buffer + tail);
      sink += buffer[tail];
      if (tail == 0) {
        break;
      }
    }
  }
  g_cache_sink.fetch_add(sink, std::memory_order_relaxed);
}

void SprayPaint::TouchPages(const uint8_t *buffer, size_t buffer_size) const {
  if (buffer == nullptr || buffer_size == 0) {
    return;
  }
  uint64_t sink = 0;
  for (size_t pos = 0; pos < buffer_size; pos += kPageSize) {
    sink += buffer[pos];
  }
  sink += buffer[buffer_size - 1];
  g_cache_sink.fetch_add(sink, std::memory_order_relaxed);
}

bool SprayPaint::RunLoadStoreStress(int round) {
  if (stress_plan_.load_store_passes == 0 || load_store_source_.empty()) {
    return true;
  }

  const size_t line_size = std::max<size_t>(1, stress_plan_.cache_line_size);
  const size_t identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 1));
  for (size_t pos = 0; pos < load_store_source_.size(); ++pos) {
    load_store_source_[pos] = static_cast<uint8_t>(
        TwoColor::Color(identity, 1, pos) ^
        static_cast<uint8_t>((pos * 13u + round) & 0xffu));
    load_store_target_[pos] = 0;
  }

  CacheHotlineBuffer(load_store_source_.data(), load_store_source_.size());

  uint64_t sink = 0;
  for (size_t pass = 0; pass < stress_plan_.load_store_passes; ++pass) {
    const uint8_t salt =
        static_cast<uint8_t>((kid_ * 29 + round * 17 + pass * 13) & 0xffu);
    for (size_t base = 0; base < load_store_source_.size(); base += line_size) {
      const size_t next = std::min(load_store_source_.size() - 1, base + line_size);
      PrefetchRead(load_store_source_.data() + next);
      PrefetchWrite(load_store_target_.data() + next);
      const size_t limit = std::min(load_store_source_.size(), base + line_size);
      for (size_t pos = base; pos < limit; ++pos) {
        const uint8_t transformed =
            StressByte(load_store_source_[pos], salt, pos + pass * line_size);
        load_store_target_[pos] = transformed;
        sink += transformed;
      }
    }

    size_t tail = load_store_target_.size();
    while (tail > 0) {
      const size_t base = tail > line_size ? tail - line_size : 0;
      for (size_t pos = base; pos < tail; ++pos) {
        const uint8_t expected =
            StressByte(load_store_source_[pos], salt, pos + pass * line_size);
        if (load_store_target_[pos] != expected) {
          SAFELOG(ERROR) << "Round: " << round_ << " kid: " << kid_
                         << " load/store mismatch at " << pos
                         << " expected: " << static_cast<int>(expected)
                         << " saw: " << static_cast<int>(load_store_target_[pos]);
          return false;
        }
        sink += load_store_target_[pos];
      }
      tail = base;
    }
    load_store_source_.swap(load_store_target_);
  }
  g_cache_sink.fetch_add(sink, std::memory_order_relaxed);
  return true;
}

bool SprayPaint::RunMadviseReclaimStress(int round) {
  if (!stress_plan_.madvise_reclaim) {
    return true;
  }

  const size_t region_size =
      RoundUpToPageSize(std::max<size_t>(kMappedBufferSize, stress_plan_.load_store_bytes));
  Mapping region = Mapping::Anonymous(region_size);
  if (!region.valid()) {
    SAFELOG(ERROR) << "mmap reclaim region failed: " << strerror(errno);
    return false;
  }

  size_t identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 2));
  PaintPattern(identity, 0, region.data(), region.size());
  if (!PatternIsRight(identity, 0, region.data(), region.size(),
                      "MadviseSeed")) {
    return false;
  }

  for (size_t pass = 0; pass < stress_plan_.madvise_passes; ++pass) {
#ifdef MADV_COLD
    AdviceIfSupported(region.data(), region.size(), MADV_COLD, "MADV_COLD");
#endif
#ifdef MADV_PAGEOUT
    AdviceIfSupported(region.data(), region.size(), MADV_PAGEOUT, "MADV_PAGEOUT");
#endif
    TouchPages(region.data(), region.size());
    if (!PatternIsRight(identity, 0, region.data(), region.size(),
                        "MadviseRefault")) {
      return false;
    }
    if (madvise(region.data(), region.size(), MADV_DONTNEED) != 0) {
      if (IsOptionalKernelPathError(errno)) {
        SAFELOG(WARN) << "MADV_DONTNEED unavailable: " << strerror(errno);
        return true;
      }
      SAFELOG(ERROR) << "MADV_DONTNEED failed: " << strerror(errno);
      return false;
    }
    TouchPages(region.data(), region.size());
    if (!BufferIsZeroFilled(region.data(), region.size(), "MadviseZeroFill")) {
      return false;
    }
    identity = EncodeIdentity(static_cast<size_t>(kid_),
                              EpochForRound(round, 3 + pass));
    PaintPattern(identity, pass + 1, region.data(), region.size());
    if (!PatternIsRight(identity, pass + 1, region.data(), region.size(),
                        "MadviseRepaint")) {
      return false;
    }
  }
  return true;
}

bool SprayPaint::RunVmaSurgeryStress(int round) {
  if (!stress_plan_.vma_surgery) {
    return true;
  }

  size_t region_size =
      RoundUpToPageSize(std::max<size_t>(4 * kPageSize, stress_plan_.load_store_bytes));
  void* mapped = mmap(nullptr, region_size, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (mapped == MAP_FAILED) {
    SAFELOG(ERROR) << "mmap VMA region failed: " << strerror(errno);
    return false;
  }

  auto* buffer = static_cast<uint8_t*>(mapped);
  size_t identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 12));
  PaintPattern(identity, 0, buffer, region_size);
  if (!PatternIsRight(identity, 0, buffer, region_size, "VmaSeed")) {
    munmap(buffer, region_size);
    return false;
  }

  for (size_t pass = 0; pass < stress_plan_.vma_passes; ++pass) {
    const size_t protect_offset = region_size > 2 * kPageSize ? kPageSize : 0;
    const size_t protect_size =
        region_size > 2 * kPageSize ? region_size - (2 * kPageSize) : kPageSize;
    if (mprotect(buffer + protect_offset, protect_size, PROT_NONE) != 0) {
      SAFELOG(ERROR) << "mprotect(PROT_NONE) failed: " << strerror(errno);
      munmap(buffer, region_size);
      return false;
    }
    if (mprotect(buffer + protect_offset, protect_size,
                 PROT_READ | PROT_WRITE) != 0) {
      SAFELOG(ERROR) << "mprotect(PROT_READ|PROT_WRITE) failed: "
                     << strerror(errno);
      munmap(buffer, region_size);
      return false;
    }
    if (!PatternIsRight(identity, 0, buffer, region_size, "VmaProtect")) {
      munmap(buffer, region_size);
      return false;
    }

    const size_t previous_size = region_size;
    const size_t grown_size = previous_size + (2 * kPageSize);
    void* remapped = SysMremap(buffer, previous_size, grown_size, MREMAP_MAYMOVE);
    if (remapped == MAP_FAILED) {
      if (IsOptionalKernelPathError(errno)) {
        SAFELOG(WARN) << "mremap unavailable: " << strerror(errno);
        munmap(buffer, previous_size);
        return true;
      }
      SAFELOG(ERROR) << "mremap grow failed: " << strerror(errno);
      munmap(buffer, previous_size);
      return false;
    }
    buffer = static_cast<uint8_t*>(remapped);
    region_size = grown_size;
    if (!BufferIsZeroFilled(buffer + previous_size, region_size - previous_size,
                            "VmaGrowZero")) {
      munmap(buffer, region_size);
      return false;
    }

    identity = EncodeIdentity(static_cast<size_t>(kid_),
                              EpochForRound(round, 13 + pass));
    PaintPattern(identity, pass + 1, buffer, region_size);
    if (!PatternIsRight(identity, pass + 1, buffer, region_size, "VmaGrow")) {
      munmap(buffer, region_size);
      return false;
    }

    if (region_size > 4 * kPageSize) {
      const size_t shrink_size = region_size - kPageSize;
      remapped = SysMremap(buffer, region_size, shrink_size, MREMAP_MAYMOVE);
      if (remapped == MAP_FAILED) {
        SAFELOG(ERROR) << "mremap shrink failed: " << strerror(errno);
        munmap(buffer, region_size);
        return false;
      }
      buffer = static_cast<uint8_t*>(remapped);
      region_size = shrink_size;
      if (!PatternIsRight(identity, pass + 1, buffer, region_size,
                          "VmaShrink")) {
        munmap(buffer, region_size);
        return false;
      }
    }
  }
  munmap(buffer, region_size);
  return true;
}

bool SprayPaint::RunProcessVmTransferStress(int round) {
  if (!stress_plan_.process_vm_transfer) {
    return true;
  }

  const size_t region_size =
      RoundUpToPageSize(std::max<size_t>(kMappedBufferSize, stress_plan_.load_store_bytes));
  Mapping remote = Mapping::Anonymous(region_size);
  Mapping reply = Mapping::Anonymous(region_size);
  if (!remote.valid() || !reply.valid()) {
    SAFELOG(ERROR) << "mmap process_vm region failed: " << strerror(errno);
    return false;
  }

  std::vector<uint8_t> source(region_size, 0);
  std::vector<uint8_t> sink(region_size, 0);
  const size_t write_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 20));
  const size_t reply_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 21));
  PaintPattern(write_identity, 0, source.data(), source.size());

  int control_fd[2];
  if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, control_fd) != 0) {
    SAFELOG(ERROR) << "socketpair process_vm failed: " << strerror(errno);
    return false;
  }
  ScopedFd parent_control(control_fd[0]);
  ScopedFd child_control(control_fd[1]);

  const pid_t helper = fork();
  if (helper < 0) {
    SAFELOG(ERROR) << "fork process_vm helper failed: " << strerror(errno);
    return false;
  }

  if (helper == 0) {
    parent_control.reset();
    char command = 0;
    if (!ReadExact(child_control.get(), &command, sizeof(command))) {
      _exit(2);
    }
    if (command == 'Q') {
      _exit(0);
    }
    if (command != 'W') {
      _exit(3);
    }
    if (!PatternIsRight(write_identity, 0, remote.data(), remote.size(),
                        "ProcessVmChildWrite")) {
      _exit(4);
    }
    PaintPattern(reply_identity, 1, reply.data(), reply.size());
    command = 'A';
    if (!WriteExact(child_control.get(), &command, sizeof(command))) {
      _exit(5);
    }
    if (!ReadExact(child_control.get(), &command, sizeof(command))) {
      _exit(6);
    }
    _exit(command == 'Q' ? 0 : 7);
  }

  child_control.reset();
  auto cleanup_helper = [&]() -> bool {
    char command = 'Q';
    WriteExact(parent_control.get(), &command, sizeof(command));
    int status = 0;
    return waitpid(helper, &status, 0) == helper && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0;
  };

  struct iovec local_write;
  local_write.iov_base = source.data();
  local_write.iov_len = source.size();
  struct iovec remote_write;
  remote_write.iov_base = remote.data();
  remote_write.iov_len = remote.size();
  const ssize_t write_rc =
      SysProcessVmWritev(helper, &local_write, 1, &remote_write, 1, 0);
  if (write_rc < 0) {
    if (IsOptionalKernelPathError(errno)) {
      SAFELOG(WARN) << "process_vm_writev unavailable: " << strerror(errno);
      cleanup_helper();
      return true;
    }
    SAFELOG(ERROR) << "process_vm_writev failed: " << strerror(errno);
    cleanup_helper();
    return false;
  }
  if (static_cast<size_t>(write_rc) != source.size()) {
    SAFELOG(ERROR) << "process_vm_writev short transfer: " << write_rc
                   << " of " << source.size();
    cleanup_helper();
    return false;
  }

  char command = 'W';
  if (!WriteExact(parent_control.get(), &command, sizeof(command))) {
    SAFELOG(ERROR) << "process_vm helper handshake failed";
    cleanup_helper();
    return false;
  }
  if (!ReadExact(parent_control.get(), &command, sizeof(command)) ||
      command != 'A') {
    SAFELOG(ERROR) << "process_vm helper acknowledge failed";
    cleanup_helper();
    return false;
  }

  struct iovec local_read;
  local_read.iov_base = sink.data();
  local_read.iov_len = sink.size();
  struct iovec remote_read;
  remote_read.iov_base = reply.data();
  remote_read.iov_len = reply.size();
  const ssize_t read_rc =
      SysProcessVmReadv(helper, &local_read, 1, &remote_read, 1, 0);
  if (read_rc < 0) {
    if (IsOptionalKernelPathError(errno)) {
      SAFELOG(WARN) << "process_vm_readv unavailable: " << strerror(errno);
      cleanup_helper();
      return true;
    }
    SAFELOG(ERROR) << "process_vm_readv failed: " << strerror(errno);
    cleanup_helper();
    return false;
  }
  if (static_cast<size_t>(read_rc) != sink.size()) {
    SAFELOG(ERROR) << "process_vm_readv short transfer: " << read_rc << " of "
                   << sink.size();
    cleanup_helper();
    return false;
  }
  if (!PatternIsRight(reply_identity, 1, sink.data(), sink.size(),
                      "ProcessVmRead")) {
    cleanup_helper();
    return false;
  }
  return cleanup_helper();
}

bool SprayPaint::RunZeroCopyPipeStress(int round) {
  if (!stress_plan_.zero_copy_pipe) {
    return true;
  }

  const size_t region_size =
      RoundUpToPageSize(std::max<size_t>(kMappedBufferSize, stress_plan_.load_store_bytes));
  Mapping source = Mapping::Anonymous(region_size);
  if (!source.valid()) {
    SAFELOG(ERROR) << "mmap zero-copy region failed: " << strerror(errno);
    return false;
  }
  const size_t identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 30));
  PaintPattern(identity, 0, source.data(), source.size());

  int pipe_a[2];
  int pipe_b[2];
  int pipe_c[2];
  if (pipe(pipe_a) != 0 || pipe(pipe_b) != 0 || pipe(pipe_c) != 0) {
    SAFELOG(ERROR) << "pipe zero-copy setup failed: " << strerror(errno);
    return false;
  }
  ScopedFd a_read(pipe_a[0]);
  ScopedFd a_write(pipe_a[1]);
  ScopedFd b_read(pipe_b[0]);
  ScopedFd b_write(pipe_b[1]);
  ScopedFd c_read(pipe_c[0]);
  ScopedFd c_write(pipe_c[1]);
  std::vector<uint8_t> tee_copy(region_size, 0);
  std::vector<uint8_t> splice_copy(region_size, 0);

  for (size_t offset = 0; offset < region_size; offset += kMaxZeroCopyChunk) {
    const size_t chunk = std::min(kMaxZeroCopyChunk, region_size - offset);
    size_t injected = 0;
    while (injected < chunk) {
      struct iovec iov;
      iov.iov_base = source.data() + offset + injected;
      iov.iov_len = chunk - injected;
      const ssize_t rc = SysVmsplice(a_write.get(), &iov, 1, 0);
      if (rc <= 0) {
        if (IsOptionalKernelPathError(errno)) {
          SAFELOG(WARN) << "vmsplice unavailable: " << strerror(errno);
          return true;
        }
        SAFELOG(ERROR) << "vmsplice failed: " << strerror(errno);
        return false;
      }
      injected += static_cast<size_t>(rc);
    }

    size_t duplicated = 0;
    while (duplicated < chunk) {
      const ssize_t rc = SysTee(a_read.get(), b_write.get(), chunk - duplicated, 0);
      if (rc <= 0) {
        if (IsOptionalKernelPathError(errno)) {
          SAFELOG(WARN) << "tee unavailable: " << strerror(errno);
          return true;
        }
        SAFELOG(ERROR) << "tee failed: " << strerror(errno);
        return false;
      }
      duplicated += static_cast<size_t>(rc);
    }

    size_t spliced = 0;
    while (spliced < chunk) {
      const ssize_t rc =
          SysSplice(a_read.get(), nullptr, c_write.get(), nullptr, chunk - spliced, 0);
      if (rc <= 0) {
        if (IsOptionalKernelPathError(errno)) {
          SAFELOG(WARN) << "splice unavailable: " << strerror(errno);
          return true;
        }
        SAFELOG(ERROR) << "splice failed: " << strerror(errno);
        return false;
      }
      spliced += static_cast<size_t>(rc);
    }

    if (!ReadExact(b_read.get(), tee_copy.data() + offset, chunk) ||
        !ReadExact(c_read.get(), splice_copy.data() + offset, chunk)) {
      SAFELOG(ERROR) << "zero-copy drain failed";
      return false;
    }
  }

  return PatternIsRight(identity, 0, tee_copy.data(), tee_copy.size(),
                        "ZeroCopyTee") &&
         PatternIsRight(identity, 0, splice_copy.data(), splice_copy.size(),
                        "ZeroCopySplice");
}

bool SprayPaint::RunMemfdAliasStress(int round) {
  if (!stress_plan_.memfd_alias) {
    return true;
  }

  ScopedFd fd(static_cast<int>(SysMemfdCreate("rainbow-alias", 0)));
  if (!fd.valid()) {
    if (IsOptionalKernelPathError(errno)) {
      SAFELOG(WARN) << "memfd_create unavailable: " << strerror(errno);
      return true;
    }
    SAFELOG(ERROR) << "memfd_create failed: " << strerror(errno);
    return false;
  }

  const size_t region_size =
      RoundUpToPageSize(std::max<size_t>(4 * kPageSize, stress_plan_.load_store_bytes));
  if (ftruncate(fd.get(), region_size) != 0) {
    SAFELOG(ERROR) << "ftruncate memfd failed: " << strerror(errno);
    return false;
  }

  Mapping shared_a =
      Mapping::File(fd.get(), region_size, PROT_READ | PROT_WRITE, MAP_SHARED);
  Mapping shared_b =
      Mapping::File(fd.get(), region_size, PROT_READ | PROT_WRITE, MAP_SHARED);
  Mapping private_c =
      Mapping::File(fd.get(), region_size, PROT_READ | PROT_WRITE, MAP_PRIVATE);
  if (!shared_a.valid() || !shared_b.valid() || !private_c.valid()) {
    SAFELOG(ERROR) << "mmap memfd aliases failed: " << strerror(errno);
    return false;
  }

  const size_t base_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 40));
  PaintPattern(base_identity, 0, shared_a.data(), shared_a.size());
  if (!PatternIsRight(base_identity, 0, shared_b.data(), shared_b.size(),
                      "MemfdSharedMirror") ||
      !PatternIsRight(base_identity, 0, private_c.data(), private_c.size(),
                      "MemfdPrivateSeed")) {
    return false;
  }

  const size_t page_count = region_size / kPageSize;
  const size_t private_page =
      ((static_cast<size_t>(round) + static_cast<size_t>(kid_)) % page_count) * kPageSize;
  const size_t private_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 41));
  PaintPattern(private_identity, 1, private_c.data() + private_page, kPageSize,
               private_page);
  if (!PatternIsRight(private_identity, 1, private_c.data() + private_page,
                      kPageSize, "MemfdPrivateCow", private_page) ||
      !PatternIsRight(base_identity, 0, shared_a.data() + private_page, kPageSize,
                      "MemfdSharedStable", private_page) ||
      !PatternIsRight(base_identity, 0, shared_b.data() + private_page, kPageSize,
                      "MemfdSharedMirrorStable", private_page)) {
    return false;
  }

  const size_t shared_page = ((private_page / kPageSize + 1) % page_count) * kPageSize;
  const size_t shared_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 42));
  PaintPattern(shared_identity, 2, shared_a.data() + shared_page, kPageSize,
               shared_page);
  return PatternIsRight(shared_identity, 2, shared_b.data() + shared_page,
                        kPageSize, "MemfdSharedUpdate", shared_page) &&
         PatternIsRight(private_identity, 1, private_c.data() + private_page,
                        kPageSize, "MemfdPrivateRetained", private_page);
}

bool SprayPaint::RunForkTreeNode(uint8_t *buffer, size_t buffer_size, int round,
                                 size_t depth, size_t lineage,
                                 size_t identity, size_t buffer_id) {
  if (!PatternIsRight(identity, buffer_id, buffer, buffer_size,
                      absl::StrFormat("ForkTreeDepth%d", depth))) {
    return false;
  }
  if (depth >= stress_plan_.fork_tree_depth) {
    return true;
  }

  for (size_t branch = 0; branch < 2; ++branch) {
    const pid_t child = fork();
    if (child < 0) {
      SAFELOG(ERROR) << "fork tree child failed: " << strerror(errno);
      return false;
    }
    if (child == 0) {
      const size_t child_lineage = (lineage << 1) | (branch + 1);
      const size_t child_epoch =
          EpochForRound(round, 50 + depth * 8 + child_lineage);
      const size_t child_buffer_id = buffer_id + branch + 1;
      const size_t child_identity =
          EncodeIdentity(static_cast<size_t>(kid_), child_epoch);
      PaintPattern(child_identity, child_buffer_id, buffer, buffer_size);
      if (!PatternIsRight(child_identity, child_buffer_id, buffer, buffer_size,
                          "ForkTreeChild")) {
        _exit(2);
      }
      _exit(RunForkTreeNode(buffer, buffer_size, round, depth + 1, child_lineage,
                            child_identity, child_buffer_id)
                ? 0
                : 1);
    }

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
      SAFELOG(ERROR) << "fork tree child failed at depth " << depth
                     << " branch " << branch;
      return false;
    }
    if (!PatternIsRight(identity, buffer_id, buffer, buffer_size,
                        "ForkTreeParent")) {
      return false;
    }
  }
  return true;
}

bool SprayPaint::RunForkTreeStress(int round) {
  if (!stress_plan_.fork_tree) {
    return true;
  }

  const size_t region_size = RoundUpToPageSize(std::max(buffer_size_, 8 * kPageSize));
  Mapping region = Mapping::Anonymous(region_size);
  if (!region.valid()) {
    SAFELOG(ERROR) << "mmap fork-tree region failed: " << strerror(errno);
    return false;
  }

  const size_t identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 45));
  PaintPattern(identity, 0, region.data(), region.size());
  return RunForkTreeNode(region.data(), region.size(), round, 0, 0, identity, 0);
}

bool SprayPaint::RunThpKsmStress(int round) {
  if (!stress_plan_.thp_ksm) {
    return true;
  }

  const size_t region_size = std::max(kHugePageSize, stress_plan_.thp_region_bytes);
  Mapping huge = Mapping::Anonymous(region_size);
  if (!huge.valid()) {
    SAFELOG(ERROR) << "mmap THP region failed: " << strerror(errno);
    return false;
  }

  const size_t thp_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 60));
  PaintPattern(thp_identity, 0, huge.data(), huge.size());
#ifdef MADV_HUGEPAGE
  AdviceIfSupported(huge.data(), huge.size(), MADV_HUGEPAGE, "MADV_HUGEPAGE");
#endif
  TouchPages(huge.data(), huge.size());
  for (size_t page = 0; page < huge.size(); page += kPageSize) {
    const size_t next = ((page / kPageSize) * 131 + round_) % (huge.size() / kPageSize);
    g_cache_sink.fetch_add(huge.data()[next * kPageSize], std::memory_order_relaxed);
  }
  if (huge.size() > 2 * kPageSize) {
    if (mprotect(huge.data() + kPageSize, kPageSize, PROT_READ) != 0 ||
        mprotect(huge.data() + kPageSize, kPageSize, PROT_READ | PROT_WRITE) != 0) {
      SAFELOG(ERROR) << "THP mprotect churn failed: " << strerror(errno);
      return false;
    }
  }
#ifdef MADV_NOHUGEPAGE
  AdviceIfSupported(huge.data(), huge.size(), MADV_NOHUGEPAGE, "MADV_NOHUGEPAGE");
#endif
  if (!PatternIsRight(thp_identity, 0, huge.data(), huge.size(), "ThpVerify")) {
    return false;
  }

  const size_t ksm_size = RoundUpToPageSize(std::min<size_t>(huge.size(), 16 * kPageSize));
  Mapping merge_a = Mapping::Anonymous(ksm_size);
  Mapping merge_b = Mapping::Anonymous(ksm_size);
  if (!merge_a.valid() || !merge_b.valid()) {
    SAFELOG(ERROR) << "mmap KSM region failed: " << strerror(errno);
    return false;
  }

  const size_t merge_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 61));
  PaintPattern(merge_identity, 1, merge_a.data(), merge_a.size());
  std::memcpy(merge_b.data(), merge_a.data(), merge_a.size());
#ifdef MADV_MERGEABLE
  AdviceIfSupported(merge_a.data(), merge_a.size(), MADV_MERGEABLE, "MADV_MERGEABLE");
  AdviceIfSupported(merge_b.data(), merge_b.size(), MADV_MERGEABLE, "MADV_MERGEABLE");
#endif
  if (!PatternIsRight(merge_identity, 1, merge_b.data(), merge_b.size(),
                      "KsmMirror")) {
    return false;
  }

  const size_t hot_page =
      ((static_cast<size_t>(round) + static_cast<size_t>(kid_)) %
       (merge_a.size() / kPageSize)) *
      kPageSize;
  const size_t diverged_identity =
      EncodeIdentity(static_cast<size_t>(kid_), EpochForRound(round, 62));
  PaintPattern(diverged_identity, 2, merge_a.data() + hot_page, kPageSize, hot_page);
#ifdef MADV_UNMERGEABLE
  AdviceIfSupported(merge_a.data(), merge_a.size(), MADV_UNMERGEABLE,
                    "MADV_UNMERGEABLE");
  AdviceIfSupported(merge_b.data(), merge_b.size(), MADV_UNMERGEABLE,
                    "MADV_UNMERGEABLE");
#endif
  return PatternIsRight(diverged_identity, 2, merge_a.data() + hot_page,
                        kPageSize, "KsmDiverged", hot_page) &&
         PatternIsRight(merge_identity, 1, merge_b.data() + hot_page, kPageSize,
                        "KsmStable", hot_page);
}

std::string SprayPaint::Ident(const std::string &phase,
                              size_t buffer_id) const {
  return absl::StrFormat("Round: %d Kid: %d Buffer: %d %s", round_, kid_,
                         buffer_id, phase);
}

std::string SprayPaint::ErrorMessage(uint8_t color, size_t position) const {
  return absl::StrFormat("BadColor: %s Position: %d", CrackColor(color),
                         position);
}

int SprayPaint::Kid(int round, int kid) {
  round_ = round;
  if (TrySetAffinity(kid - 1)) {
    SetKid(kid);

    for (int k = 0; k < 2; k++) {
      if (!ColorIsRight("CheckPapa")) {
        SAFELOG(ERROR) << "Papa buffer came colored wrong";
        return 1;
      }
    }

    CowPoke();
    if (!ColorIsRight("PagePromote")) {
      SAFELOG(ERROR) << "Promoted buffer colored wrong";
      return 1;
    }

    last_painted_by_ = kid_;
    current_epoch_ = EpochForRound(round, static_cast<size_t>(kid));

    Paint();  // Repaint primary buffer in kid's colors.

    if (!ColorIsRight("FirstCheckMe")) {
      SAFELOG(ERROR) << "Failed to color kid: " << kid_ << " buffer right";
      return 1;
    }

    CacheHotlineBuffer(buffer_, buffer_size_);
    if (!RunLoadStoreStress(round)) {
      SAFELOG(ERROR) << "Load/store stress failed for kid: " << kid_;
      return 1;
    }
    if (!RunMadviseReclaimStress(round)) {
      SAFELOG(ERROR) << "madvise reclaim stress failed for kid: " << kid_;
      return 1;
    }
    if (!RunVmaSurgeryStress(round)) {
      SAFELOG(ERROR) << "VMA surgery stress failed for kid: " << kid_;
      return 1;
    }
    if (!RunProcessVmTransferStress(round)) {
      SAFELOG(ERROR) << "process_vm stress failed for kid: " << kid_;
      return 1;
    }
    if (!RunZeroCopyPipeStress(round)) {
      SAFELOG(ERROR) << "zero-copy pipe stress failed for kid: " << kid_;
      return 1;
    }
    if (!RunMemfdAliasStress(round)) {
      SAFELOG(ERROR) << "memfd alias stress failed for kid: " << kid_;
      return 1;
    }
    if (!RunForkTreeStress(round)) {
      SAFELOG(ERROR) << "fork-tree stress failed for kid: " << kid_;
      return 1;
    }
    if (!RunThpKsmStress(round)) {
      SAFELOG(ERROR) << "THP/KSM stress failed for kid: " << kid_;
      return 1;
    }

    if (mprotect(buffer_, buffer_size_, PROT_READ | PROT_WRITE) != 0) {
      SAFELOG(ERROR) << "mprotect: " << strerror(errno);
      return 1;
    }

    std::vector<uint8_t *> mapping;
    mapping.reserve(kMappings);
    for (size_t k = 0; k < kMappings; k++) {
      uint8_t *p = MappedBuffer(k);
      if (!p) {
        SAFELOG(ERROR) << "Round: " << round << " kid: " << kid
                       << " failed map: " << k;
        return 1;
      }
      mapping.push_back(p);
    }

    bool failed = false;
    int fd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fd) < 0) {
      SAFELOG(FATAL) << "socketpair kid: " << kid << strerror(errno);
    }
    std::thread w_thread([this, round, fd] { Writer(round, fd[0]); });
    std::thread r_thread(
        [this, round, fd, &failed] { failed = Reader(round, fd[1]); });
    w_thread.join();
    r_thread.join();
    if (failed) {
      SAFELOG(ERROR) << "Round: " << round << " kid: " << kid
                     << " failed loopback";
      return 1;
    }

    for (size_t buffer_id = 0; buffer_id < mapping.size(); buffer_id++) {
      if (!ColorIsRight(buffer_id, mapping[buffer_id], kMappedBufferSize,
                        "MapCheck")) {
        SAFELOG(ERROR) << "Failed to color kid: " << kid_
                       << " map: " << buffer_id << " right";
        return 1;
      }
      if (munmap(mapping[buffer_id], kMappedBufferSize)) {
        SAFELOG(ERROR) << "munmap: " << strerror(errno);
        return 1;
      }
    }

    if (!ColorIsRight("FinalCheckMe")) {
      SAFELOG(ERROR) << "Color faded, kid: " << kid_;
      return 1;
    }
  }
  return 0;
}

std::string SprayPaint::CrackColor(uint8_t color) const {
  return TwoColor::CrackColor(CurrentIdentity(), color);
}

std::string SprayPaint::DescribeIdentity(const TwoColor::Identity& identity) const {
  if (!stress_plan_.epoch_coloring || stress_plan_.epoch_modulus <= 1 ||
      stress_plan_.epoch_stride == 0) {
    return identity.ToString();
  }
  const size_t owner = identity.identity % stress_plan_.epoch_stride;
  const size_t epoch = identity.identity / stress_plan_.epoch_stride;
  return absl::StrFormat(
      "Identity: %d Owner: %d Epoch: %d Length: %d Phase: %d",
      identity.identity, owner, epoch, identity.length, identity.phase);
}

Summarizer::Summarizer(const std::string &ident, const SprayPaint *spray_paint,
                       const uint8_t *buffer)
    : ident_(ident), spray_paint_(spray_paint), buffer_(buffer) {
  Clear();
}

void Summarizer::Clear() {
  for (size_t k = 0; k < 256; k++) histogram_[k] = 0;
  range_fails_ = 0;
}

std::string Summarizer::Summary() const {
  if (!active_) return "";
  std::vector<std::string> v;
  const size_t range_length = range_end_ - range_start_ + 1;
  v.push_back(absl::StrFormat(
      "Range: %d Range start: %d Range end: %d Length: %d Range fails: %d %s "
      "Colors:",
      range_count_, range_start_, range_end_, range_length, range_fails_,
      IsSquelched() ? "Squelched" : ""));
  for (size_t k = 0; k < 256; k++) {
    if (histogram_[k]) {
      v.push_back(absl::StrFormat("  %s: %9d", spray_paint_->CrackColor(k),
                                  histogram_[k]));
    }
  }
  SAFELOG(INFO) << "Identifying";
  auto r = TwoColor::Identify(buffer_ + range_start_, range_length);
  if (r.has_value()) {
    v.push_back(spray_paint_->DescribeIdentity(*r));
  } else {
    v.push_back("Identity indeterminate");
  }
  return absl::StrJoin(v, "\n");
}

void Summarizer::Report(size_t position, uint8_t color,
                        const std::string &error) {
  std::string summary;
  total_fails_++;
  if (!IsSquelched()) {
    if (active_ && position != range_end_ + 1) {
      // Start a new range, disgorging the previous range.
      SAFELOG(ERROR) << ident_ << " " << Summary();
      range_count_++;
      Clear();
      range_start_ = position;
    }
    SAFELOG(ERROR) << ident_ << " " << error;
  }
  if (!active_) {
    range_count_ = 1;
    range_start_ = position;
    active_ = true;
  }
  range_end_ = position;
  range_fails_++;
  histogram_[color]++;
}

void Summarizer::Finish() const {
  if (active_) {
    SAFELOG(ERROR) << ident_ << " " << Summary();
  }
}

bool Summarizer::IsSquelched() const {
  return total_fails_ >= static_cast<int64_t>(kSpewLimit);
}
}  // namespace gvisor
