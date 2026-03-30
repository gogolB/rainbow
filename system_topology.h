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

struct StressPlan {
  size_t cache_line_size = 64;
  bool cache_hotline = false;
  size_t cache_hotline_passes = 0;
  size_t load_store_bytes = 0;
  size_t load_store_passes = 0;
  bool prescan_applied = false;

  std::string Summary() const;
};

std::optional<SystemTopology> PrescanSystemTopology();

StressPlan BuildStressPlan(size_t buffer_size,
                           const std::optional<SystemTopology>& topology,
                           bool enable_cache_hotline,
                           size_t cache_hotline_passes,
                           size_t requested_load_store_bytes,
                           size_t requested_load_store_passes);

bool ParseCacheSizeBytes(const std::string& text, size_t* bytes);
size_t CountCpuListEntries(const std::string& text);

}  // namespace gvisor

#endif  // SYSTEM_TOPOLOGY_H_
