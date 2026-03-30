# Rainbow

Rainbow screen test is a Silent Data Corruption(SDC) test. The test attempts to
find CPU hardware faults by exercising privilege code in kernel and hypervisor.

The basic operation of the test is as follows:

* Do little work in user space, push the drama up into kernel or hypervisor.
* Provoke with Fork, COW page promotion, mmap and mprotect, Looping back data through pipes
* Data provenance is marked by assigning each process a unique recognizable data pattern.


## Prerequisites:

Designed to run under Unix/Linux OS.

* cmake: https://cmake.org/
* Abseil-cpp: https://github.com/abseil/abseil-cpp


## Building

```
sh$ git clone https://github.com/google/rainbow.git
sh$ cd rainbow
sh$ mkdir build
sh$ cd build
sh$ cmake ..
sh$ cmake --build . --target rainbow
```
Note that rainbow must be built with the C++17 standard.

### Docker build

The repository includes a multi-stage `Dockerfile` that builds a static
`rainbow` binary and runs the test suite inside the builder stage.

```
sh$ docker build -t rainbow:static .
sh$ sh ./export-static.sh rainbow:static ./rainbow
```

## Prescan and SDC stress

Rainbow now supports a prescan-only mode plus opt-in kernel-mediated stress
paths that are aimed at silent data corruption rather than pure compute load.
The current modes cover:

* deterministic load/store verification
* reclaim and refault via `madvise`
* VMA churn via `mprotect` and `mremap`
* cross-process transfer via `process_vm_writev` and `process_vm_readv`
* zero-copy transfer via `vmsplice`, `tee`, and `splice`
* `memfd` aliasing with shared/private mappings
* nested fork-tree copy-on-write ancestry checks
* THP and KSM-oriented mapping churn

Because this tool can destabilize a machine, start with `--prescan_only`,
then a short run with `--kids=1`, and only enable the heavier knobs when the
host stays stable.

Useful flags:

* `--prescan_only`: print detected CPU/cache topology and the derived stress plan.
* `--prescan_topology=true|false`: enable or disable Linux `/sys` topology scan.
* `--cache_hotline=true|false`: warm cache lines before validation. Disabled by
  default for safer bring-up.
* `--cache_hotline_passes=N`: number of cache hotlining passes per buffer.
* `--epoch_coloring=true|false`: encode owner epochs into the color identity so
  stale copies can be distinguished from wrong-owner copies.
* `--epoch_stride=N`: reserve this many identity slots per epoch. `0` derives a
  safe value from the prescan results.
* `--load_store_passes=N`: number of extra deterministic load/store passes per
  child. Disabled by default for safer bring-up.
* `--load_store_bytes=BYTES`: working-set size for the load/store phase; `0`
  derives a value from the prescan results.
* `--madvise_reclaim=true|false`: run reclaim and refault verification with
  `MADV_COLD`, `MADV_PAGEOUT`, and `MADV_DONTNEED`.
* `--madvise_passes=N`: number of reclaim/refault passes.
* `--vma_surgery=true|false`: run `mprotect` and `mremap` growth/shrink/move
  verification.
* `--vma_passes=N`: number of VMA churn passes.
* `--process_vm_transfer=true|false`: run cross-process
  `process_vm_writev/process_vm_readv` verification.
* `--zero_copy_pipe=true|false`: run `vmsplice`/`tee`/`splice` verification.
* `--memfd_alias=true|false`: run shared/private alias verification on a `memfd`.
* `--fork_tree=true|false`: run nested fork/COW ancestry verification.
* `--fork_tree_depth=N`: depth of the nested fork tree.
* `--thp_ksm=true|false`: run THP and KSM-oriented mapping churn.
* `--thp_region_bytes=BYTES`: region size for the THP/KSM path; `0` derives a
  huge-page-sized region from the prescan results.

Example:

```
sh$ ./build/rainbow --prescan_only
sh$ ./build/rainbow --run_time=30s --kids=1
sh$ ./build/rainbow --run_time=30s --kids=8 --load_store_passes=8 \
       --cache_hotline=true --cache_hotline_passes=2 \
       --load_store_bytes=$((4 * 1024 * 1024))
sh$ ./build/rainbow --run_time=10s --kids=1 --epoch_coloring=true \
       --madvise_reclaim=true --vma_surgery=true --process_vm_transfer=true \
       --zero_copy_pipe=true --memfd_alias=true --fork_tree=true \
       --fork_tree_depth=1 --thp_ksm=true
```

## CI and release

GitHub Actions now builds the project through the Docker container on pull
requests, pushes to `main`, and tag pushes matching `v*`.

On tag pushes, the workflow exports the static binary and uploads it to the
GitHub Releases page so teammates can download the artifact directly.

## Future updates

* Update instruction to run rainbow with gVisor to provoke hypervisor.
