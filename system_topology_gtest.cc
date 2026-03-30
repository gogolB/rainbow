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

  const StressPlan plan = BuildStressPlan(4096, topology, true, 3, 0, 0);
  EXPECT_TRUE(plan.prescan_applied);
  EXPECT_EQ(plan.cache_line_size, 64);
  EXPECT_TRUE(plan.cache_hotline);
  EXPECT_EQ(plan.cache_hotline_passes, 3);
  EXPECT_EQ(plan.load_store_passes, 0);
  EXPECT_EQ(plan.load_store_bytes, 2 * 1024 * 1024);
}

TEST(SystemTopology, BuildStressPlanHonorsExplicitOverrides) {
  const StressPlan plan =
      BuildStressPlan(4096, std::nullopt, false, 99, 300000, 7);
  EXPECT_FALSE(plan.prescan_applied);
  EXPECT_FALSE(plan.cache_hotline);
  EXPECT_EQ(plan.cache_hotline_passes, 0);
  EXPECT_EQ(plan.load_store_passes, 7);
  EXPECT_EQ(plan.load_store_bytes % plan.cache_line_size, 0);
}

}  // namespace
}  // namespace gvisor
