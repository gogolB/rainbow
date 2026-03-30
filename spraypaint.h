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

#ifndef SPRAYPAINT_H_
#define SPRAYPAINT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "system_topology.h"
#include "two_color.h"

namespace gvisor {

// Paints and validates protection-domain-specific data patterns, creates and
// manipulates buffers and their access rights, and pumps loopback data
// through enclosing kernel or hypervisor.
class SprayPaint {
 public:
  explicit SprayPaint(size_t buffer_size, StressPlan stress_plan = {});
  virtual ~SprayPaint();

  // Checks and exercises, intended for child processes.
  int Kid(int round, int kid);
  int GetKid() const { return kid_; }

  // Returns true if 'buffer_' is correctly colored.
  bool ColorIsRight(const std::string &ident) const;
  bool ColorIsRight(size_t buffer_id, const uint8_t *buffer, size_t buffer_size,
                    const std::string &ident) const;

  // Returns decoded color (as modular values), or 'Garbage' if invalid.
  std::string CrackColor(uint8_t color) const;
  std::string DescribeIdentity(const TwoColor::Identity& identity) const;
  const uint8_t *buffer() const { return buffer_; }
  size_t buffer_size() const { return buffer_size_; }

  // Accessor to class static variable kMappedBufferSize
  static const size_t GetMappedBufferSize() { return kMappedBufferSize; }

  // Try setting thread affinity to a specific core
  virtual bool TrySetAffinity(int lpu);

 private:
  // Maximum chunk size for via-socket copy. More than a page seems reasonable.
  static constexpr size_t kMaxTransfer = 4127;

  static constexpr size_t kMappings = 503;

  static const size_t kMappedBufferSize;

  void SetKid(int kid);

  // Promotes buffer from copy-on-write to writable, changing no data.
  void CowPoke();

  // Paints 'buffer_' according to the color scheme.
  void Paint();

  // Writes buffer_ to socket 'fd' in random-length sequential chunks.
  void Writer(int round, int fd) const;

  // Returns true if socket 'fd' does not faithfully disgorge buffer_ content.
  bool Reader(int round, int fd) const;

  // Returns a painted mapped buffer of size kMappedBufferSize.
  // Returns null_ptr on error.
  uint8_t *MappedBuffer(size_t id) const;

  size_t CurrentIdentity() const;
  size_t EpochForRound(int round, size_t salt = 0) const;
  size_t EncodeIdentity(size_t owner, size_t epoch) const;
  void PaintPattern(size_t identity, size_t buffer_id, uint8_t *buffer,
                    size_t buffer_size, size_t base_position = 0) const;
  bool PatternIsRight(size_t identity, size_t buffer_id, const uint8_t *buffer,
                      size_t buffer_size, const std::string &ident,
                      size_t base_position = 0) const;
  bool BufferIsZeroFilled(const uint8_t *buffer, size_t buffer_size,
                          const std::string &ident) const;
  void CacheHotlineBuffer(const uint8_t *buffer, size_t buffer_size) const;
  void TouchPages(const uint8_t *buffer, size_t buffer_size) const;
  bool RunLoadStoreStress(int round);
  bool RunMadviseReclaimStress(int round);
  bool RunVmaSurgeryStress(int round);
  bool RunProcessVmTransferStress(int round);
  bool RunZeroCopyPipeStress(int round);
  bool RunMemfdAliasStress(int round);
  bool RunForkTreeStress(int round);
  bool RunForkTreeNode(uint8_t *buffer, size_t buffer_size, int round,
                       size_t depth, size_t lineage, size_t identity,
                       size_t buffer_id);
  bool RunThpKsmStress(int round);

  std::string Ident(const std::string &phase, size_t buffer_id) const;

  std::string ErrorMessage(uint8_t color, size_t position) const;

  const size_t buffer_size_;
  const StressPlan stress_plan_;
  int round_ = 0;
  int kid_ = 0;
  int last_painted_by_ = 0;
  size_t current_epoch_ = 0;
  uint8_t *buffer_;
  std::vector<uint8_t> load_store_source_;
  std::vector<uint8_t> load_store_target_;
};

// Summarizes and logs corruption, tries to avoid giant log spew.
class Summarizer {
 public:
  Summarizer(const std::string &ident, const SprayPaint *spray_paint,
             const uint8_t *buffer);

  void Report(size_t position, uint8_t color, const std::string &error);

  void Finish() const;

  bool IsSquelched() const;

  size_t range_count() const { return range_count_; }
  size_t total_fails() const { return total_fails_; }

 private:
  static constexpr size_t kSpewLimit = 600;

  void Clear();
  std::string Summary() const;

  const std::string ident_;
  const SprayPaint *const spray_paint_;
  const uint8_t *const buffer_;
  bool active_ = false;
  size_t range_count_ = 0;
  size_t range_start_;
  size_t range_end_;
  size_t range_fails_ = 0;
  int64_t total_fails_ = 0;
  int64_t histogram_[256];
};
}  // namespace gvisor
#endif  // SPRAYPAINT_H_
