# Intel Memory Latency Checker (MLC) Project Overview

This directory contains the original Intel Memory Latency Checker (MLC v3.13) release,
reverse-engineered artifacts, and a compilable clean-room C implementation.

## Top-Level Entries in mlc:

* intel:
  The official prebuilt binary directory provided by Intel. It contains the original stripped
64-bit ELF executable ('intel/mlc') and the redistribution license notice.

* src:
  The compilable C source code directory. It includes:
  - 'mlc.c': A clean-room, standard C implementation conforming to MLC specifications (supporting
  idle latency, latency matrix across NUMA nodes, multi-threaded AVX2 bandwidth tests, and CLI options).
  - 'Makefile': Build configuration for compiling mlc.c.
  - 'readme_mlc_v3.13.rst': Detailed Intel MLC v3.13 technical documentation and usage guide.

* decompiled_raw:
  The raw reverse-engineered C pseudocode archive generated via REA (Ghidra 12.1.4 engine).
  It contains the full decompilation of all 412 internal functions from 'intel/mlc' (divided
  into 'procedures_part1.c' through 'procedures_part9.c', 'main_decompiled.c', 'usage_decompiled.c',
  and 'ghidra_types.h') for reference and deep micro-architectural auditing.

* Makefile:
  The top-level build script. Running 'make' triggers the build in 'src' and generates the
runnable 'mlc_rebuilt' binary in the 'mlc/' directory.

* mlc_v3.13.tgz:
  The original Intel MLC v3.13 compressed archive package.
