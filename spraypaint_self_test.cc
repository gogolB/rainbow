#include "spraypaint.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

class AffinedSprayPaint : public gvisor::SprayPaint {
 public:
  using gvisor::SprayPaint::SprayPaint;

  bool TrySetAffinity(int) override { return true; }
};

class UnaffinedSprayPaint : public gvisor::SprayPaint {
 public:
  using gvisor::SprayPaint::SprayPaint;

  bool TrySetAffinity(int) override { return false; }
};

}  // namespace

int main() {
  bool ok = true;

  gvisor::StressPlan stress_plan;
  stress_plan.cache_line_size = 64;
  stress_plan.cache_hotline = true;
  stress_plan.cache_hotline_passes = 2;
  stress_plan.load_store_bytes = 256 * 1024;
  stress_plan.load_store_passes = 2;

  AffinedSprayPaint stressed(8192, stress_plan);
  ok &= Expect(stressed.ColorIsRight("SelfTestCtor"),
               "Primary buffer should be correctly painted after construction");
  ok &= Expect(stressed.Kid(3, 1) == 0,
               "Kid should succeed with load/store stress and cache hotlining enabled");

  UnaffinedSprayPaint skipped(8192, stress_plan);
  ok &= Expect(skipped.Kid(3, 1) == 0,
               "Kid should remain a no-op when affinity cannot be set");

  if (!ok) {
    return EXIT_FAILURE;
  }
  std::cout << "spraypaint_self_test passed\n";
  return EXIT_SUCCESS;
}
