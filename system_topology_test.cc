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

  gvisor::StressOptions derived_options;
  derived_options.enable_cache_hotline = true;
  derived_options.cache_hotline_passes = 3;
  derived_options.enable_madvise_reclaim = true;
  derived_options.madvise_passes = 2;
  derived_options.enable_memfd_alias = true;
  derived_options.enable_fork_tree = true;
  derived_options.fork_tree_depth = 2;
  derived_options.enable_thp_ksm = true;
  const gvisor::StressPlan derived =
      gvisor::BuildStressPlan(4096, topology, derived_options);
  ok &= Expect(derived.prescan_applied, "Derived plan should record prescan use");
  ok &= Expect(derived.cache_line_size == 64,
               "Derived plan should use the detected cache line size");
  ok &= Expect(derived.cache_hotline && derived.cache_hotline_passes == 3,
               "Derived plan should honor cache hotlining settings");
  ok &= Expect(derived.epoch_coloring && derived.epoch_modulus >= 2,
               "Derived plan should enable epoch coloring when identity space allows it");
  ok &= Expect(derived.load_store_passes == 0,
               "Derived plan should leave load/store stress disabled unless requested");
  ok &= Expect(derived.load_store_bytes == 2 * 1024 * 1024,
               "Derived plan should size the working set from the largest private cache");
  ok &= Expect(derived.madvise_reclaim && derived.madvise_passes == 2,
               "Derived plan should honor reclaim stress settings");
  ok &= Expect(derived.memfd_alias,
               "Derived plan should carry memfd alias stress settings");
  ok &= Expect(derived.fork_tree && derived.fork_tree_depth == 2,
               "Derived plan should clamp fork-tree depth into the plan");
  ok &= Expect(derived.thp_ksm && derived.thp_region_bytes == 2 * 1024 * 1024,
               "Derived plan should derive a THP/KSM-sized region");

  gvisor::StressOptions explicit_options;
  explicit_options.enable_cache_hotline = false;
  explicit_options.cache_hotline_passes = 9;
  explicit_options.requested_load_store_bytes = 300000;
  explicit_options.requested_load_store_passes = 7;
  explicit_options.enable_epoch_coloring = false;
  explicit_options.enable_vma_surgery = true;
  explicit_options.vma_passes = 4;
  explicit_options.enable_process_vm_transfer = true;
  explicit_options.enable_zero_copy_pipe = true;
  explicit_options.enable_thp_ksm = true;
  explicit_options.requested_thp_region_bytes = 3 * 1024 * 1024;
  const gvisor::StressPlan explicit_plan =
      gvisor::BuildStressPlan(4096, std::nullopt, explicit_options);
  ok &= Expect(!explicit_plan.prescan_applied,
               "Explicit plan without prescan should report prescan off");
  ok &= Expect(!explicit_plan.cache_hotline && explicit_plan.cache_hotline_passes == 0,
               "Explicit plan should disable cache hotlining cleanly");
  ok &= Expect(!explicit_plan.epoch_coloring && explicit_plan.epoch_modulus == 1,
               "Explicit plan should allow epoch coloring to be disabled");
  ok &= Expect(explicit_plan.load_store_passes == 7,
               "Explicit plan should honor explicit load/store passes");
  ok &= Expect(explicit_plan.load_store_bytes % explicit_plan.cache_line_size == 0,
               "Explicit plan should align the working set to cache lines");
  ok &= Expect(explicit_plan.vma_surgery && explicit_plan.vma_passes == 4,
               "Explicit plan should honor VMA surgery settings");
  ok &= Expect(explicit_plan.process_vm_transfer && explicit_plan.zero_copy_pipe,
               "Explicit plan should carry transfer mode settings");
  ok &= Expect(explicit_plan.thp_region_bytes == 4 * 1024 * 1024,
               "Explicit plan should align the THP region to huge-page size");

  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "system_topology_test passed\n";
  return EXIT_SUCCESS;
}
