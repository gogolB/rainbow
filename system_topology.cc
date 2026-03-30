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
constexpr size_t kDefaultCacheLineSize = 64;
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

std::string StressPlan::Summary() const {
  std::ostringstream out;
  out << "prescan=" << (prescan_applied ? "on" : "off")
      << " line_size=" << cache_line_size
      << " cache_hotline=" << (cache_hotline ? "on" : "off")
      << " hotline_passes=" << cache_hotline_passes
      << " load_store_bytes=" << FormatBytes(load_store_bytes)
      << " load_store_passes=" << load_store_passes;
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
                           bool enable_cache_hotline,
                           size_t cache_hotline_passes,
                           size_t requested_load_store_bytes,
                           size_t requested_load_store_passes) {
  StressPlan plan;
  plan.prescan_applied = topology.has_value();
  plan.cache_line_size = topology.has_value()
                             ? std::max(kDefaultCacheLineSize,
                                        topology->MaxCacheLineSize())
                             : kDefaultCacheLineSize;
  plan.cache_hotline = enable_cache_hotline;
  plan.cache_hotline_passes = enable_cache_hotline
                                  ? std::max<size_t>(1, cache_hotline_passes)
                                  : 0;
  plan.load_store_passes = requested_load_store_passes;

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

  if (requested_load_store_bytes != 0) {
    plan.load_store_bytes = requested_load_store_bytes;
  } else {
    plan.load_store_bytes = recommended_bytes;
  }
  plan.load_store_bytes =
      AlignUp(std::max(plan.load_store_bytes, buffer_size), plan.cache_line_size);
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
