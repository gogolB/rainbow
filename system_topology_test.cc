#include "system_topology.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  size_t size_bytes = 0;
  ok &= Expect(gvisor::ParseCacheSizeBytes("32K", &size_bytes) &&
                   size_bytes == 32 * 1024,
               "ParseCacheSizeBytes should parse KiB values");
  ok &= Expect(gvisor::ParseCacheSizeBytes("8M", &size_bytes) &&
                   size_bytes == 8 * 1024 * 1024,
               "ParseCacheSizeBytes should parse MiB values");
  ok &= Expect(gvisor::CountCpuListEntries("0-5,24-29") == 12,
               "CountCpuListEntries should expand comma/range cpu lists");

  gvisor::SystemTopology topology;
  topology.hardware_threads = 24;
  topology.online_cpus = 24;
  topology.caches.push_back(
      gvisor::CacheInfo{1, "Data", 32 * 1024, 64, 2, "0,24"});
  topology.caches.push_back(
      gvisor::CacheInfo{2, "Unified", 1024 * 1024, 64, 2, "0,24"});
  topology.caches.push_back(gvisor::CacheInfo{
      3, "Unified", 32 * 1024 * 1024, 64, 12, "0-5,24-29"});

  const gvisor::StressPlan derived =
      gvisor::BuildStressPlan(4096, topology, true, 3, 0, 0);
  ok &= Expect(derived.prescan_applied, "Derived plan should record prescan use");
  ok &= Expect(derived.cache_line_size == 64,
               "Derived plan should use the detected cache line size");
  ok &= Expect(derived.cache_hotline && derived.cache_hotline_passes == 3,
               "Derived plan should honor cache hotlining settings");
  ok &= Expect(derived.load_store_passes == 0,
               "Derived plan should leave load/store stress disabled unless requested");
  ok &= Expect(derived.load_store_bytes == 2 * 1024 * 1024,
               "Derived plan should size the working set from the largest private cache");

  const gvisor::StressPlan explicit_plan =
      gvisor::BuildStressPlan(4096, std::nullopt, false, 9, 300000, 7);
  ok &= Expect(!explicit_plan.prescan_applied,
               "Explicit plan without prescan should report prescan off");
  ok &= Expect(!explicit_plan.cache_hotline && explicit_plan.cache_hotline_passes == 0,
               "Explicit plan should disable cache hotlining cleanly");
  ok &= Expect(explicit_plan.load_store_passes == 7,
               "Explicit plan should honor explicit load/store passes");
  ok &= Expect(explicit_plan.load_store_bytes % explicit_plan.cache_line_size == 0,
               "Explicit plan should align the working set to cache lines");

  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "system_topology_test passed\n";
  return EXIT_SUCCESS;
}
