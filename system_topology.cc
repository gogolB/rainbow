#include "system_topology.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace gvisor {
namespace {

constexpr size_t kKiB = 1024;
constexpr size_t kMiB = 1024 * kKiB;
constexpr size_t kHugePageSize = 2 * kMiB;
constexpr size_t kDefaultCacheLineSize = 64;
constexpr size_t kTwoColorIdentitySpace = 29 * 31;
constexpr char kCpuOnlinePath[] = "/sys/devices/system/cpu/online";
constexpr char kCacheRootPath[] = "/sys/devices/system/cpu/cpu0/cache";

size_t AlignUp(size_t value, size_t alignment) {
  if (alignment == 0) {
    return value;
  }
  return ((value + alignment - 1) / alignment) * alignment;
}

std::string Trim(std::string text) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  text.erase(text.begin(),
             std::find_if(text.begin(), text.end(), not_space));
  text.erase(
      std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
  return text;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return Trim(buffer.str());
}

std::optional<size_t> ParseSize(const std::filesystem::path& path) {
  const std::optional<std::string> text = ReadFile(path);
  if (!text.has_value()) {
    return std::nullopt;
  }
  size_t value = 0;
  if (!ParseCacheSizeBytes(*text, &value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<int> ParseInt(const std::filesystem::path& path) {
  const std::optional<std::string> text = ReadFile(path);
  if (!text.has_value()) {
    return std::nullopt;
  }
  try {
    return std::stoi(*text);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<CacheInfo> ReadCacheInfo(const std::filesystem::path& path) {
  CacheInfo info;
  const auto level = ParseInt(path / "level");
  const auto type = ReadFile(path / "type");
  const auto size_bytes = ParseSize(path / "size");
  const auto line_size = ParseSize(path / "coherency_line_size");
  const auto shared_cpu_list = ReadFile(path / "shared_cpu_list");
  if (!level.has_value() || !type.has_value() || !size_bytes.has_value()) {
    return std::nullopt;
  }
  info.level = *level;
  info.type = *type;
  info.size_bytes = *size_bytes;
  info.line_size = line_size.value_or(kDefaultCacheLineSize);
  info.shared_cpu_list = shared_cpu_list.value_or("");
  info.shared_cpu_count =
      std::max<size_t>(1, CountCpuListEntries(info.shared_cpu_list));
  return info;
}

bool IsPrivateCache(const CacheInfo& cache) { return cache.shared_cpu_count <= 2; }

std::string FormatBytes(size_t bytes) {
  std::ostringstream out;
  if (bytes >= kMiB && (bytes % kMiB) == 0) {
    out << (bytes / kMiB) << " MiB";
  } else if (bytes >= kKiB && (bytes % kKiB) == 0) {
    out << (bytes / kKiB) << " KiB";
  } else {
    out << bytes << " B";
  }
  return out.str();
}

}  // namespace

size_t SystemTopology::MaxCacheLineSize() const {
  size_t max_line = 0;
  for (const CacheInfo& cache : caches) {
    max_line = std::max(max_line, cache.line_size);
  }
  return max_line;
}

size_t SystemTopology::LargestPrivateCache() const {
  size_t largest = 0;
  for (const CacheInfo& cache : caches) {
    if ((cache.type == "Data" || cache.type == "Unified") && IsPrivateCache(cache)) {
      largest = std::max(largest, cache.size_bytes);
    }
  }
  return largest;
}

size_t SystemTopology::LargestSharedCache() const {
  size_t largest = 0;
  for (const CacheInfo& cache : caches) {
    if ((cache.type == "Data" || cache.type == "Unified") &&
        cache.shared_cpu_count > 2) {
      largest = std::max(largest, cache.size_bytes);
    }
  }
  return largest;
}

std::string SystemTopology::Summary() const {
  std::ostringstream out;
  out << "hardware_threads=" << hardware_threads
      << " online_cpus=" << online_cpus;
  for (const CacheInfo& cache : caches) {
    out << " [L" << cache.level << ' ' << cache.type
        << " size=" << FormatBytes(cache.size_bytes)
        << " line=" << cache.line_size
        << " shared=" << cache.shared_cpu_count;
    if (!cache.shared_cpu_list.empty()) {
      out << " cpus=" << cache.shared_cpu_list;
    }
    out << "]";
  }
  return out.str();
}

bool StressPlan::HasKernelSdcModes() const {
  return madvise_reclaim || vma_surgery || process_vm_transfer ||
         zero_copy_pipe || memfd_alias || fork_tree || thp_ksm;
}

std::string StressPlan::Summary() const {
  std::ostringstream out;
  out << "prescan=" << (prescan_applied ? "on" : "off")
      << " line_size=" << cache_line_size
      << " cache_hotline=" << (cache_hotline ? "on" : "off")
      << " hotline_passes=" << cache_hotline_passes
      << " epoch_coloring=" << (epoch_coloring ? "on" : "off")
      << " epoch_stride=" << epoch_stride
      << " epoch_modulus=" << epoch_modulus
      << " load_store_bytes=" << FormatBytes(load_store_bytes)
      << " load_store_passes=" << load_store_passes;
  if (HasKernelSdcModes()) {
    out << " modes=[";
    bool first = true;
    auto append_mode = [&](const std::string& mode) {
      if (!first) {
        out << ',';
      }
      out << mode;
      first = false;
    };
    if (madvise_reclaim) {
      append_mode("madvise:" + std::to_string(madvise_passes));
    }
    if (vma_surgery) {
      append_mode("vma:" + std::to_string(vma_passes));
    }
    if (process_vm_transfer) {
      append_mode("process_vm");
    }
    if (zero_copy_pipe) {
      append_mode("zero_copy_pipe");
    }
    if (memfd_alias) {
      append_mode("memfd_alias");
    }
    if (fork_tree) {
      append_mode("fork_tree:" + std::to_string(fork_tree_depth));
    }
    if (thp_ksm) {
      append_mode("thp_ksm:" + FormatBytes(thp_region_bytes));
    }
    out << "]";
  }
  return out.str();
}

std::optional<SystemTopology> PrescanSystemTopology() {
  SystemTopology topology;
  topology.hardware_threads = std::thread::hardware_concurrency();

  const std::optional<std::string> online = ReadFile(kCpuOnlinePath);
  topology.online_cpus =
      online.has_value() ? CountCpuListEntries(*online) : topology.hardware_threads;

  std::error_code ec;
  const std::filesystem::path cache_root(kCacheRootPath);
  if (std::filesystem::exists(cache_root, ec)) {
    for (const auto& entry : std::filesystem::directory_iterator(cache_root, ec)) {
      if (ec || !entry.is_directory()) {
        continue;
      }
      const std::string name = entry.path().filename().string();
      if (name.rfind("index", 0) != 0) {
        continue;
      }
      const auto info = ReadCacheInfo(entry.path());
      if (info.has_value()) {
        topology.caches.push_back(*info);
      }
    }
  }

  std::sort(topology.caches.begin(), topology.caches.end(),
            [](const CacheInfo& left, const CacheInfo& right) {
              if (left.level != right.level) {
                return left.level < right.level;
              }
              return left.type < right.type;
            });

  if (topology.hardware_threads == 0 && topology.online_cpus == 0 &&
      topology.caches.empty()) {
    return std::nullopt;
  }
  if (topology.online_cpus == 0) {
    topology.online_cpus = topology.hardware_threads;
  }
  return topology;
}

StressPlan BuildStressPlan(size_t buffer_size,
                           const std::optional<SystemTopology>& topology,
                           const StressOptions& options) {
  StressPlan plan;
  plan.prescan_applied = topology.has_value();
  plan.cache_line_size = topology.has_value()
                             ? std::max(kDefaultCacheLineSize,
                                        topology->MaxCacheLineSize())
                             : kDefaultCacheLineSize;
  plan.cache_hotline = options.enable_cache_hotline;
  plan.cache_hotline_passes = options.enable_cache_hotline
                                  ? std::max<size_t>(1, options.cache_hotline_passes)
                                  : 0;
  plan.load_store_passes = options.requested_load_store_passes;

  size_t recommended_bytes = buffer_size;
  if (topology.has_value()) {
    const size_t private_cache = topology->LargestPrivateCache();
    const size_t shared_cache = topology->LargestSharedCache();
    if (private_cache != 0) {
      recommended_bytes = private_cache * 2;
    } else if (shared_cache != 0 && topology->online_cpus != 0) {
      recommended_bytes = shared_cache / topology->online_cpus;
    }
  }
  recommended_bytes = std::max(recommended_bytes, 256 * kKiB);
  recommended_bytes = std::min(recommended_bytes, 8 * kMiB);

  if (options.requested_load_store_bytes != 0) {
    plan.load_store_bytes = options.requested_load_store_bytes;
  } else {
    plan.load_store_bytes = recommended_bytes;
  }
  plan.load_store_bytes =
      AlignUp(std::max(plan.load_store_bytes, buffer_size), plan.cache_line_size);

  const size_t concurrency = std::max<size_t>(
      1, topology.has_value()
             ? std::max(topology->online_cpus, topology->hardware_threads)
             : std::thread::hardware_concurrency());
  size_t epoch_stride = options.requested_epoch_stride;
  if (epoch_stride == 0) {
    epoch_stride = std::max<size_t>(32, concurrency + 8);
  }
  epoch_stride = std::min(epoch_stride, kTwoColorIdentitySpace - 1);
  plan.epoch_stride = std::max<size_t>(1, epoch_stride);
  plan.epoch_modulus =
      std::max<size_t>(1, (kTwoColorIdentitySpace - 1) / plan.epoch_stride);
  plan.epoch_coloring = options.enable_epoch_coloring && plan.epoch_modulus > 1;
  if (!plan.epoch_coloring) {
    plan.epoch_modulus = 1;
  }

  plan.madvise_reclaim = options.enable_madvise_reclaim;
  plan.madvise_passes =
      plan.madvise_reclaim ? std::max<size_t>(1, options.madvise_passes) : 0;
  plan.vma_surgery = options.enable_vma_surgery;
  plan.vma_passes =
      plan.vma_surgery ? std::max<size_t>(1, options.vma_passes) : 0;
  plan.process_vm_transfer = options.enable_process_vm_transfer;
  plan.zero_copy_pipe = options.enable_zero_copy_pipe;
  plan.memfd_alias = options.enable_memfd_alias;
  plan.fork_tree = options.enable_fork_tree;
  plan.fork_tree_depth =
      plan.fork_tree ? std::min<size_t>(3, std::max<size_t>(1, options.fork_tree_depth))
                     : 0;
  plan.thp_ksm = options.enable_thp_ksm;
  if (plan.thp_ksm) {
    size_t thp_bytes = options.requested_thp_region_bytes;
    if (thp_bytes == 0) {
      thp_bytes = std::max(recommended_bytes, kHugePageSize);
    }
    plan.thp_region_bytes = AlignUp(std::max(thp_bytes, kHugePageSize), kHugePageSize);
  }
  return plan;
}

bool ParseCacheSizeBytes(const std::string& text, size_t* bytes) {
  if (bytes == nullptr) {
    return false;
  }
  const std::string trimmed = Trim(text);
  if (trimmed.empty()) {
    return false;
  }
  size_t index = 0;
  unsigned long long value = 0;
  try {
    value = std::stoull(trimmed, &index);
  } catch (...) {
    return false;
  }
  size_t multiplier = 1;
  if (index < trimmed.size()) {
    switch (std::toupper(trimmed[index])) {
      case 'K':
        multiplier = kKiB;
        break;
      case 'M':
        multiplier = kMiB;
        break;
      case 'G':
        multiplier = 1024ULL * kMiB;
        break;
      default:
        return false;
    }
    ++index;
  }
  if (index != trimmed.size()) {
    return false;
  }
  *bytes = static_cast<size_t>(value * multiplier);
  return true;
}

size_t CountCpuListEntries(const std::string& text) {
  const std::string trimmed = Trim(text);
  if (trimmed.empty()) {
    return 0;
  }

  size_t total = 0;
  size_t start = 0;
  while (start < trimmed.size()) {
    const size_t comma = trimmed.find(',', start);
    const std::string token =
        Trim(trimmed.substr(start, comma == std::string::npos
                                       ? std::string::npos
                                       : comma - start));
    if (!token.empty()) {
      const size_t dash = token.find('-');
      try {
        if (dash == std::string::npos) {
          (void)std::stoul(token);
          total += 1;
        } else {
          const size_t left = std::stoul(token.substr(0, dash));
          const size_t right = std::stoul(token.substr(dash + 1));
          if (right >= left) {
            total += right - left + 1;
          }
        }
      } catch (...) {
        return 0;
      }
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
  }
  return total;
}

}  // namespace gvisor
