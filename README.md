# MadFS

[![workflow](https://github.com/shawnzhong/MadFS/actions/workflows/test.yml/badge.svg)](https://github.com/ShawnZhong/MadFS/actions/workflows/test.yml)
[![workflow](https://github.com/shawnzhong/MadFS/actions/workflows/bench.yml/badge.svg)](https://github.com/ShawnZhong/MadFS/actions/workflows/bench.yml)
[![workflow](https://github.com/shawnzhong/MadFS/actions/workflows/format.yml/badge.svg)](https://github.com/ShawnZhong/MadFS/actions/workflows/format.yml)

Source code for the paper published at FAST '23:

"MadFS: Per-File Virtualization for Userspace Persistent Memory Filesystems" by
Shawn Zhong, Chenhao Ye, Guanzhou Hu, Suyan Qu, Andrea Arpaci-Dusseau, Remzi
Arpaci-Dusseau, and Michael Swift.
[FAST '23 Link](https://www.usenix.org/conference/fast23/presentation/zhong)

## Paper Abstract

Persistent memory (PM) can be accessed directly from userspace without kernel
involvement, but most PM filesystems still perform metadata operations in the
kernel for security and rely on the kernel for cross-process synchronization.

We present per-file virtualization, where a virtualization layer implements a
complete set of file functionalities, including metadata management, crash
consistency, and concurrency control, in userspace. We observe that not all file
metadata need to be maintained by the kernel and propose embedding insensitive
metadata into the file for userspace management. For crash consistency,
copy-on-write (CoW) benefits from the embedding of the block mapping since the
mapping can be efficiently updated without kernel involvement. For cross-process
synchronization, we introduce lock-free optimistic concurrency control (OCC) at
user level, which tolerates process crashes and provides better scalability.

Based on per-file virtualization, we implement MadFS, a library PM filesystem
that maintains the embedded metadata as a compact log. Experimental results show
that on concurrent workloads, MadFS achieves up to 3.6× the throughput of
ext4-DAX. For real-world applications, MadFS provides up to 48% speedup for YCSB
on LevelDB and 85% for TPC-C on SQLite compared to NOVA.

## Prerequisites

- MadFS is developed on Ubuntu 20.04.3 LTS and Ubuntu 22.04.1 LTS. It should
  work on other Linux distributions as well.

- MadFS requires a C++ compiler with C++ 20 support. The compilers known to work
  includes GCC 11.3.0, GCC 10.3.0, Clang 14.0.0, and Clang
  10.0.0.

- <details>
  <summary>Install dependencies and configure the system</summary>

    - Install build dependencies

      ```shell
      sudo apt update
      sudo apt install -y cmake build-essential gcc-10 g++-10
      ```

    - Install development dependencies (optional)

      ```shell
      # to run sanitizers and formatter
      sudo apt install -y clang-10 libstdc++-10-dev clang-format-10
      # for perf
      sudo apt install -y linux-tools-common linux-tools-generic linux-tools-`uname -r`
      # for managing persistent memory and NUMA
      sudo apt install -y ndctl numactl
      # for benchmarking
      sudo apt install -y sqlite3
      ```

    - Configure the system

      ```shell
      ./scripts/init.py
      ```
  </details>

- <details>
  <summary>Configure persistent memory</summary>

    - To emulate a persistent memory device using DRAM, please follow the
      guide [here](https://docs.pmem.io/persistent-memory/getting-started-guide/creating-development-environments/linux-environments/linux-memmap).

    - Initialize namespaces (optional)
      ```shell
      # remove existing namespaces on region0
      sudo ndctl destroy-namespace all --region=region0 --force 
      # create new namespace `/dev/pmem0` on region0
      sudo ndctl create-namespace --region=region0 --size=20G
      # create new namespace `/dev/pmem0.1` on region0 for NOVA (optional)
      sudo ndctl create-namespace --region=region0 --size=20G
      # list all namespaces
      ndctl list --region=0 --namespaces --human --idle
      ```

    - Use `/dev/pmem0` to mount ext4-DAX at `/mnt/pmem0-ext4-dax`
      ```shell
      # create filesystem
      sudo mkfs.ext4 /dev/pmem0
      # create mount point
      sudo mkdir -p /mnt/pmem0-ext4-dax
      # mount filesystem
      sudo mount -o dax /dev/pmem0 /mnt/pmem0-ext4-dax
      # make the mount point writable
      sudo chmod a+w /mnt/pmem0-ext4-dax
      # check mount status
      mount -v | grep /mnt/pmem0-ext4-dax
      ```

    - Use `/dev/pmem0.1` to mount NOVA at `/mnt/pmem0-nova` (optional)
      ```shell
      # load NOVA module
      sudo modprobe nova
      # create mount point
      sudo mkdir -p /mnt/pmem0-nova
      # mount filesystem
      sudo mount -t NOVA -o init -o data_cow  /dev/pmem0.1 /mnt/pmem0-nova
      # make the mount point writable
      sudo chmod a+w /mnt/pmem0-nova           
      # check mount status
      mount -v | grep /mnt/pmem0-nova          
      ```

    - To unmount the filesystems, run
      ```shell
      sudo umount /mnt/pmem0-ext4-dax
      sudo umount /mnt/pmem0-nova
      ```

  </details>

## Build and Run

- Build the MadFS shared library

  ```shell
  # Usage: make [release|debug|relwithdebinfo|profile|pmemcheck|asan|ubsan|msan|tsan]
  #             [CMAKE_ARGS="-DKEY1=VAL1 -DKEY2=VAL2 ..."] 
  make BUILD_TARGETS="madfs"
  ```

- Run your program with MadFS

  ```shell
  LD_PRELOAD=./build-release/libmadfs.so ./your_program
  ```

- Run tests

  ```
  ./scripts/run.py [test_basic|test_rc|test_sync|test_gc]
  ```

-   <details> 
    <summary>Run and plot single-threaded benchmarks </summary>

    ```shell
    ./scripts/bench_st.py --filter="seq_pread"
    ./scripts/bench_st.py --filter="rnd_pread"
    ./scripts/bench_st.py --filter="seq_pwrite"
    ./scripts/bench_st.py --filter="rnd_pwrite"
    ./scripts/bench_st.py --filter="cow"
    ./scripts/bench_st.py --filter="append_pwrite"
    
    # Limit to set of file systems
    ./scripts/bench_st.py -f MadFS SplitFS
    
    # Profile a data point
    ./scripts/bench_st.py --filter="seq_pread/512" -f MadFS -b profile
    ```
    </details>

-   <details>
    <summary>Run and plot multi-threaded benchmarks</summary>

    ```shell
    ./scripts/bench_mt.py --filter="unif_0R"
    ./scripts/bench_mt.py --filter="unif_50R"
    ./scripts/bench_mt.py --filter="unif_95R"
    ./scripts/bench_mt.py --filter="unif_100R"
    ./scripts/bench_mt.py --filter="zipf_2k"
    ./scripts/bench_mt.py --filter="zipf_4k"
    ```
    </details>

-   <details>
    <summary>Run and plot metadata benchmarks</summary>

    ```shell
    ./scripts/bench_open.py
    ./scripts/bench_gc.py
    ```
    </details>

-   <details>
    <summary>Run and plot macrobenchmarks (SQLite and LevelDB) </summary>

    ```shell
    ./scripts/bench_tpcc.py
    ./scripts/bench_ycsb.py
    ```
    </details>
