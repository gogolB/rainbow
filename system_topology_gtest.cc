#include "system_topology.h"

#include <cstddef>
#include <optional>

#include "gtest/gtest.h"

namespace gvisor {
namespace {

TEST(SystemTopology, ParseCacheSizeBytes) {
  size_t bytes = 0;
  EXPECT_TRUE(ParseCacheSizeBytes("32K", &bytes));
  EXPECT_EQ(bytes, 32 * 1024);
  EXPECT_TRUE(ParseCacheSizeBytes("8M", &bytes));
  EXPECT_EQ(bytes, 8 * 1024 * 1024);
  EXPECT_FALSE(ParseCacheSizeBytes("nope", &bytes));
}

TEST(SystemTopology, CountCpuListEntries) {
  EXPECT_EQ(CountCpuListEntries("0-5,24-29"), 12);
  EXPECT_EQ(CountCpuListEntries("7"), 1);
  EXPECT_EQ(CountCpuListEntries(""), 0);
}

TEST(SystemTopology, BuildStressPlanUsesPrivateCache) {
  SystemTopology topology;
  topology.hardware_threads = 24;
  topology.online_cpus = 24;
  topology.caches.push_back(CacheInfo{1, "Data", 32 * 1024, 64, 2, "0,24"});
  topology.caches.push_back(CacheInfo{2, "Unified", 1024 * 1024, 64, 2, "0,24"});
  topology.caches.push_back(
      CacheInfo{3, "Unified", 32 * 1024 * 1024, 64, 12, "0-5,24-29"});

  StressOptions options;
  options.enable_cache_hotline = true;
  options.cache_hotline_passes = 3;
  options.enable_madvise_reclaim = true;
  options.madvise_passes = 2;
  options.enable_memfd_alias = true;
  options.enable_fork_tree = true;
  options.fork_tree_depth = 2;
  options.enable_thp_ksm = true;

  const StressPlan plan = BuildStressPlan(4096, topology, options);
  EXPECT_TRUE(plan.prescan_applied);
  EXPECT_EQ(plan.cache_line_size, 64);
  EXPECT_TRUE(plan.cache_hotline);
  EXPECT_EQ(plan.cache_hotline_passes, 3);
  EXPECT_TRUE(plan.epoch_coloring);
  EXPECT_GE(plan.epoch_modulus, 2);
  EXPECT_EQ(plan.load_store_passes, 0);
  EXPECT_EQ(plan.load_store_bytes, 2 * 1024 * 1024);
  EXPECT_TRUE(plan.madvise_reclaim);
  EXPECT_EQ(plan.madvise_passes, 2);
  EXPECT_TRUE(plan.memfd_alias);
  EXPECT_TRUE(plan.fork_tree);
  EXPECT_EQ(plan.fork_tree_depth, 2);
  EXPECT_TRUE(plan.thp_ksm);
  EXPECT_EQ(plan.thp_region_bytes, 2 * 1024 * 1024);
}

TEST(SystemTopology, BuildStressPlanHonorsExplicitOverrides) {
  StressOptions options;
  options.enable_cache_hotline = false;
  options.cache_hotline_passes = 99;
  options.requested_load_store_bytes = 300000;
  options.requested_load_store_passes = 7;
  options.enable_epoch_coloring = false;
  options.enable_vma_surgery = true;
  options.vma_passes = 4;
  options.enable_process_vm_transfer = true;
  options.enable_zero_copy_pipe = true;
  options.requested_thp_region_bytes = 3 * 1024 * 1024;
  options.enable_thp_ksm = true;

  const StressPlan plan = BuildStressPlan(4096, std::nullopt, options);
  EXPECT_FALSE(plan.prescan_applied);
  EXPECT_FALSE(plan.cache_hotline);
  EXPECT_EQ(plan.cache_hotline_passes, 0);
  EXPECT_FALSE(plan.epoch_coloring);
  EXPECT_EQ(plan.epoch_modulus, 1);
  EXPECT_EQ(plan.load_store_passes, 7);
  EXPECT_EQ(plan.load_store_bytes % plan.cache_line_size, 0);
  EXPECT_TRUE(plan.vma_surgery);
  EXPECT_EQ(plan.vma_passes, 4);
  EXPECT_TRUE(plan.process_vm_transfer);
  EXPECT_TRUE(plan.zero_copy_pipe);
  EXPECT_EQ(plan.thp_region_bytes, 4 * 1024 * 1024);
}

}  // namespace
}  // namespace gvisor
