#ifndef SYSTEM_TOPOLOGY_H_
#define SYSTEM_TOPOLOGY_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace gvisor {

struct CacheInfo {
  int level = 0;
  std::string type;
  size_t size_bytes = 0;
  size_t line_size = 64;
  size_t shared_cpu_count = 1;
  std::string shared_cpu_list;
};

struct SystemTopology {
  size_t hardware_threads = 0;
  size_t online_cpus = 0;
  std::vector<CacheInfo> caches;

  size_t MaxCacheLineSize() const;
  size_t LargestPrivateCache() const;
  size_t LargestSharedCache() const;
  std::string Summary() const;
};

struct StressOptions {
  bool enable_cache_hotline = false;
  size_t cache_hotline_passes = 0;
  bool enable_epoch_coloring = true;
  size_t requested_epoch_stride = 0;
  size_t requested_load_store_bytes = 0;
  size_t requested_load_store_passes = 0;
  bool enable_madvise_reclaim = false;
  size_t madvise_passes = 1;
  bool enable_vma_surgery = false;
  size_t vma_passes = 1;
  bool enable_process_vm_transfer = false;
  bool enable_zero_copy_pipe = false;
  bool enable_memfd_alias = false;
  bool enable_fork_tree = false;
  size_t fork_tree_depth = 1;
  bool enable_thp_ksm = false;
  size_t requested_thp_region_bytes = 0;
};

struct StressPlan {
  size_t cache_line_size = 64;
  bool cache_hotline = false;
  size_t cache_hotline_passes = 0;
  bool epoch_coloring = true;
  size_t epoch_stride = 32;
  size_t epoch_modulus = 1;
  size_t load_store_bytes = 0;
  size_t load_store_passes = 0;
  bool madvise_reclaim = false;
  size_t madvise_passes = 0;
  bool vma_surgery = false;
  size_t vma_passes = 0;
  bool process_vm_transfer = false;
  bool zero_copy_pipe = false;
  bool memfd_alias = false;
  bool fork_tree = false;
  size_t fork_tree_depth = 0;
  bool thp_ksm = false;
  size_t thp_region_bytes = 0;
  bool prescan_applied = false;

  bool HasKernelSdcModes() const;
  std::string Summary() const;
};

std::optional<SystemTopology> PrescanSystemTopology();

StressPlan BuildStressPlan(size_t buffer_size,
                           const std::optional<SystemTopology>& topology,
                           const StressOptions& options);

bool ParseCacheSizeBytes(const std::string& text, size_t* bytes);
size_t CountCpuListEntries(const std::string& text);

}  // namespace gvisor

#endif  // SYSTEM_TOPOLOGY_H_
