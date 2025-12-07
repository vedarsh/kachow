# USRL: Ultra-Low Latency Shared Ring Library

## Executive Summary

**USRL** (Ultra-Low Latency Shared Ring Library) is a high-performance, zero-copy inter-process communication (IPC) framework designed for real-time embedded systems, avionics, robotics, and high-frequency trading applications. It now features a **Hybrid Transport Layer** combining Shared Memory (SHM) for local ultra-low latency and TCP for network distribution.

### Key Metrics
- **SHM Latency:** ~78 nanoseconds per message
- **SHM Throughput:** 12.8+ Million messages/sec (SWMR)
- **SHM Bandwidth:** 28.6 GB/s (for 8KB payloads)
- **TCP Throughput:** 12.3 Gbps (10GbE saturation)
- **Overhead:** Sub-microsecond (stays in CPU L3 cache)
- **Scalability:** Linear performance scaling with multiple writers

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Installation & Build](#installation--build)
3. [Configuration](#configuration)
4. [API Reference](#api-reference)
5. [Usage Examples](#usage-examples)
6. [Benchmark Results](#benchmark-results)
7. [Best Practices](#best-practices)
8. [Troubleshooting](#troubleshooting)

---

## Architecture Overview

### Design Philosophy

USRL implements **lock-free ring buffers** using atomic compare-and-swap (CAS) operations to achieve deterministic, sub-microsecond latency without kernel involvement. It extends this with a **Network Transport Layer** for distributed systems.

### Key Components

#### 1. **Core Memory Manager** (`usrl_core`)
- Allocates and manages shared memory segment (`/dev/shm/usrl_core`)
- Hosts multiple independent ring buffers
- Memory size: Configurable (64MB - 512MB typical)

#### 2. **SWMR Ring (Single-Writer, Multi-Reader)**
- Optimized for single publisher, multiple subscribers
- Zero contention on write path
- Best for sensor data streaming (IMU, GPS)

#### 3. **MWMR Ring (Multi-Writer, Multi-Reader)**
- Multiple publishers safely competing for the same channel
- Lock-free using atomic `fetch_add`
- Best for command aggregation / logging

#### 4. **Network Transport** (`usrl_net`)
- **TCP:** Reliable, blocking stream transport (12+ Gbps)
- **Architecture:** Client/Server model with `usrl_trans_send` / `usrl_trans_recv`
- **Future:** Zero-copy bridge to SHM rings

### Memory Layout (SHM)

```
â”Œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”
â”‚  Shared Memory Segment                  |
â”‚  Size: N MB (typically 64-256 MB)       â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚  Core Metadata (1KB)                    â”‚
â”‚  - Magic Number, Topic Count            â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚  Topic Descriptors (N * 512 bytes)      â”‚
â”‚  - Ring offsets, Slot count/size        â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚  Ring Buffer 1: SWMR "sensor_imu"       â”‚
â”‚  - Head/Tail pointers (atomic)          â”‚
â”‚  - Slot 0..N [Payload...]               â”‚
â”œâ”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”¤
â”‚  Ring Buffer 2: MWMR "command_queue"    â”‚
â”‚  - Head/Tail (atomic)                   â”‚
â”‚  - Slot 0..N [Payload...]               â”‚
â””â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
```

---

## Installation & Build

### Prerequisites

**OS:** Linux (x86-64, ARM64)
**Compiler:** GCC 9+ or Clang 10+, CMake 3.10+
**Libs:** `libpthread`

### Build Steps

1. **Clone USRL**
```bash
git clone https://github.com/your-repo/usrl-core.git
cd usrl-core
```

2. **Build (Release Mode)**
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

3. **Verify Build**
```bash
ls -1 benchmarks/
# init_bench
# bench_pub_swmr
# bench_sub
# bench_tcp_server
# bench_tcp_client
```

---

## Configuration

### Configuration File: `usrl_config_bench.json`

Located in `benchmarks/`. Defines SHM topics and Network buffer sizes.

```json
{
  "memory_size_mb": 512,
  "topics": [
    {
      "name": "sensor_imu",
      "slots": 8192,
      "payload_size": 64,
      "type": "swmr"
    },
    {
      "name": "video_stream",
      "slots": 128,
      "payload_size": 921600,
      "type": "swmr"
    },
    {
      "name": "tcp_buffer",
      "slots": 4096,
      "payload_size": 4096,
      "type": "swmr"
    }
  ]
}
```

---

## API Reference

### 1. Core API (`usrl_core.h`)

*   **`usrl_core_init(path, size, topics, count)`**: Creates SHM segment.
*   **`usrl_core_map(path, size)`**: Maps existing SHM segment.

### 2. SHM Pub/Sub (`usrl_ring.h`)

*   **`usrl_pub_init(pub, core, topic, id)`**: Setup SWMR publisher.
*   **`usrl_pub_publish(pub, data, len)`**: Write message (Zero-Copy internal).
*   **`usrl_mwmr_pub_init(...)`**: Setup MWMR publisher.
*   **`usrl_sub_init(sub, core, topic)`**: Setup subscriber.
*   **`usrl_sub_next(sub, buf, len, id)`**: Non-blocking read.

### 3. Network Transport (`usrl_net.h`)

*   **`usrl_trans_create(type, host, port, ...)`**: Create TCP Client/Server.
*   **`usrl_trans_accept(server, client_out)`**: Accept incoming connection.
*   **`usrl_trans_send(ctx, data, len)`**: Blocking send.
*   **`usrl_trans_recv(ctx, data, len)`**: Blocking recv.

---

## Usage Examples

### Example 1: SHM Pub/Sub (Low Latency)

**Publisher:**
```c
UsrlPublisher pub;
usrl_pub_init(&pub, core, "sensor_imu", 1);
while(1) {
    usrl_pub_publish(&pub, data, 64);
    usleep(1000); // 1kHz
}
```

**Subscriber:**
```c
UsrlSubscriber sub;
usrl_sub_init(&sub, core, "sensor_imu");
while(1) {
    if (usrl_sub_next(&sub, buf, 64, &id) > 0) {
        process(buf);
    }
}
```

### Example 2: TCP Network (High Throughput)

**Server:**
```c
usrl_transport_t *srv = usrl_trans_create(USRL_TRANS_TCP, NULL, 8080, ...);
usrl_transport_t *client;
usrl_trans_accept(srv, &client);

while(1) {
    usrl_trans_recv(client, buf, 4096); // Wait for req
    usrl_trans_send(client, buf, 4096); // Echo resp
}
```

**Client:**
```c
usrl_transport_t *cli = usrl_trans_create(USRL_TRANS_TCP, "127.0.0.1", 8080, ...);
usrl_trans_send(cli, req, 4096);
usrl_trans_recv(cli, resp, 4096);
```

---

## Benchmark Results

Benchmarks performed on **Linux (Loopback/SHM)**.

### 1. Shared Memory (SHM) Performance

| Test | Payload | Throughput | Bandwidth | Latency |
| :--- | :--- | :--- | :--- | :--- |
| **Small Ring (SWMR)** | 64 Bytes | **12.80 M msg/s** | 781 MB/s | **78.13 ns** |
| **MWMR Standard** | 64 Bytes | **12.05 M msg/s** | 735 MB/s | **83.01 ns** |
| **Huge Message** | 8 KB | **3.66 M msg/s** | **28.6 GB/s** | 272 ns |

### 2. Network (TCP) Performance

| Test | Payload | Rate | Throughput | Latency |
| :--- | :--- | :--- | :--- | :--- |
| **TCP Loopback** | 4 KB | **0.38 M req/s** | **12.33 Gbps** | 2.65 Âµs |

### 3. Comparison vs Alternatives

| Metric | USRL (SHM) | USRL (TCP) | Unix Pipe | ROS 2 (Iceoryx) |
| :--- | :--- | :--- | :--- | :--- |
| **Latency** | **78 ns** | 2.6 Âµs | ~5000 ns | ~500 ns |
| **Throughput** | **12.8 M/s** | 0.38 M/s | 0.2 M/s | 1-3 M/s |
| **Bandwidth** | **28 GB/s** | 1.5 GB/s | 50 MB/s | 100-500 MB/s |

---

## Best Practices

1.  **SHM Sizing:** Ensure `slots * slot_size * num_topics < total_memory`.
2.  **Blocking vs Non-Blocking:**
    *   **SHM:** Use busy-wait loops for subscribers requiring <1Âµs latency.
    *   **TCP:** Blocking I/O (`send/recv`) is recommended for dedicated links (12Gbps verified).
3.  **Core Affinity:** Pin Publisher/Subscriber threads to isolated CPU cores (`taskset -c 2 ./pub`) for 0-jitter performance.

---

## Troubleshooting

1.  **TCP 0 Throughput?**
    *   Ensure Server/Client use **Identical Payload Sizes**. Mismatches cause blocking deadlocks.
2.  **SHM Init Failed?**
    *   Run `rm /dev/shm/usrl_core`. Old segments with different sizes cause conflicts.
3.  **High Latency Spikes?**
    *   Disable CPU power saving: `sudo cpupower frequency-set -g performance`.

---

**USRL v1.1 - Validated December 2025**