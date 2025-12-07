# USRL: Ultra-Low Latency Shared Ring Library

USRL is a high-performance, lock-free inter-process communication (IPC) library based on shared memory ring buffers. It is designed for systems that require deterministic, sub-microsecond latency and high sustained throughput, such as avionics, robotics, and high-frequency trading.

USRL eliminates kernel involvement from the hot path by using shared memory and C11 atomics, enabling processes to exchange data at memory speed rather than socket or pipe speed.[web:3][web:4]  

---

## Key Characteristics

- Ultra-low latency message passing (typical single-hop latency on the order of hundreds of nanoseconds).
- High message throughput (multi-million messages per second on commodity CPUs).
- Shared-memory transport with zero-copy semantics.
- Lock-free SWMR (Single-Writer, Multi-Reader) and MWMR (Multi-Writer, Multi-Reader) ring primitives.
- Static topic configuration via JSON, with explicit control over ring size and payload size.
- Small, self-contained C API suitable for embedded and safety-critical environments.

---

## Architecture Overview

USRL exposes a single shared-memory “core” region that contains:

- Global metadata and topic descriptors.
- One or more SWMR or MWMR rings, each bound to a named topic.
- Atomic head/tail indices per ring, accessed via C11 atomics.

Publishers and subscribers:

- Map the same shared-memory segment.
- Locate ring descriptors by topic name.
- Use non-blocking atomic operations to publish and consume messages.

This architecture avoids kernel-mediated IPC (sockets, pipes, message queues) and associated context switches, which are well-known to add microsecond-scale overhead per message.[web:5][web:7]  

---

## Build and Installation

### Prerequisites

- Linux (x86_64 or ARM64)
- CMake 3.16 or newer
- GCC 9+ or Clang 10+ with C11 support
- POSIX threads and `librt`

### Build Steps

For Build check out `USRL_Documentation.md`.
To run Benchmarks run `./benchmarks.sh`
