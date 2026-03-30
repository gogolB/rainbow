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
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <random>
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

ABSL_FLAG(bool, ignore_affinity_failure, false,
          "Silently ignore affinity failure");

namespace gvisor {
namespace {
const size_t kPageSize = sysconf(_SC_PAGESIZE);
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

void SprayPaint::CowPoke() {
  for (size_t k = 0; k < buffer_size_; k += kPageSize) {
    const size_t page = k / kPageSize;
    const size_t offset =
        (page * 131 + stress_plan_.cache_line_size) % kPageSize;
    const size_t kk = k + offset;
    buffer_[kk] = TwoColor::Color(last_painted_by_, 0, kk);
  }
}

void SprayPaint::Paint() {
  TwoColor::Paint(last_painted_by_, 0, buffer_, buffer_size_);
}

bool SprayPaint::ColorIsRight(const std::string &ident) const {
  return ColorIsRight(0, buffer_, buffer_size_, ident);
}

bool SprayPaint::ColorIsRight(size_t buffer_id, const uint8_t *buffer,
                              size_t buffer_size,
                              const std::string &ident) const {
  bool ok = true;
  Summarizer summarizer(Ident(ident, buffer_id), this, buffer);
  for (size_t k = 0; k < buffer_size; k++) {
    const uint8_t color = buffer[k];
    if (color != TwoColor::Color(last_painted_by_, buffer_id, k)) {
      summarizer.Report(k, color, ErrorMessage(color, k));
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
      summarizer.Report(k, color, ErrorMessage(color, k));
    }
    buffer[k] = TwoColor::Color(last_painted_by_, id, k);
  }
  summarizer.Finish();
  if (dirty) {
    SAFELOG(ERROR) << "Dirty map for kid: " << kid_;
    munmap(buffer, kMappedBufferSize);
    return nullptr;
  }
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

bool SprayPaint::RunLoadStoreStress(int round) {
  if (stress_plan_.load_store_passes == 0 || load_store_source_.empty()) {
    return true;
  }

  const size_t line_size = std::max<size_t>(1, stress_plan_.cache_line_size);
  const size_t identity = static_cast<size_t>(kid_ + 1 + round * 7);
  for (size_t pos = 0; pos < load_store_source_.size(); ++pos) {
    load_store_source_[pos] = static_cast<uint8_t>(
        TwoColor::Color(identity, round % TwoColor::kPeriod, pos) ^
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
  return TwoColor::CrackColor(kid_, color);
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
  constexpr size_t kThreshold = 6;
  if (r.has_value()) {
    if (r->identity != static_cast<size_t>(spray_paint_->GetKid()) &&
        r->identity != 0 &&
        r->length > kThreshold) {
      v.push_back(absl::StrFormat("*** Indiscretion %s from Kid: %d Length: %d",
                                  ident_, r->identity, r->length));
    } else {
      v.push_back(r->ToString());
    }
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
