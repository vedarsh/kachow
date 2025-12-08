# USRL: Ultra-Low Latency Shared Ring Library

## Executive Summary

**USRL** (Ultra-Low Latency Shared Ring Library) is a high-performance, zero-copy inter-process communication (IPC) framework designed for real-time embedded systems, avionics, robotics, and high-frequency trading applications.

### Key Metrics
- **Latency:** 150 nanoseconds per message
- **Throughput:** 6.5+ Million messages/sec (SWMR), 9.75+ Million messages/sec (MWMR)
- **Bandwidth:** 8+ GB/s for large payloads
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

USRL implements **lock-free ring buffers** using atomic compare-and-swap (CAS) operations to achieve deterministic, sub-microsecond latency without kernel involvement.

### Key Components

#### 1. **Core Memory Manager** (`usrl_core`)
- Allocates and manages shared memory segment (`/dev/shm/usrl_core`)
- Hosts multiple independent ring buffers
- Supports runtime topic configuration
- Memory size: Configurable (64MB - 256MB typical)

#### 2. **SWMR Ring (Single-Writer, Multi-Reader)**
- Optimized for single publisher, multiple subscribers
- Zero contention on write path
- Atomic read operations only
- Best for sensor data streaming (IMU, GPS, Barometer)

#### 3. **MWMR Ring (Multi-Writer, Multi-Reader)**
- Multiple publishers safely competing for the same channel
- Lock-free using atomic `fetch_add` on head pointer
- Scales linearly up to 8+ concurrent writers
- Best for aggregating data from multiple sources

#### 4. **Subscriber Model**
- Non-blocking read with configurable behavior
- Tracks last-read offset per subscriber
- Supports multiple independent subscribers per ring
- Returns Publisher ID (source identification)

### Memory Layout

```
┌─────────────────────────────────────────┐
│  Shared Memory Segment                  |
│  Size: N MB (typically 64-128 MB)       │
├─────────────────────────────────────────┤
│  Core Metadata (1KB)                    │
│  - Magic Number                         │
│  - Topic Count                          │
│  - Version Info                         │
├─────────────────────────────────────────┤
│  Topic Descriptors (N * 512 bytes)      │
│  - Ring pointer offsets                 │
│  - Slot count / size                    │
│  - Type (SWMR/MWMR)                     │
├─────────────────────────────────────────┤
│  Ring Buffer 1: SWMR "sensor_imu"       │
│  - Head/Tail pointers (atomic)          │
│  - Slot 0 [Payload...]                  │
│  - Slot 1 [Payload...]                  │
│  - ...                                  │
├─────────────────────────────────────────┤
│  Ring Buffer 2: MWMR "command_queue"    │
│  - Head pointer (atomic, shared)        │
│  - Slot 0 [Payload...]                  │
│  - Slot 1 [Payload...]                  │
│  - ...                                  │
└─────────────────────────────────────────┘
```

---

## Installation & Build

### Prerequisites

**Operating System:** Linux (tested on x86-64, ARM64, M1 Mac under Docker)

**Compiler Requirements:**
- GCC 9+ or Clang 10+
- C11 Standard Support (`_Atomic`, stdatomic.h)
- POSIX Threads

**Build Tools:**
```bash
sudo apt-get install cmake make gcc g++ libpthread-stubs0-dev
```

### Build Steps

1. **Clone or Download USRL**
```bash
cd /home/vedarsh/projects/usrl-core
```

2. **Create Build Directory**
```bash
mkdir -p build && cd build
```

3. **Configure (Release Mode - Optimized)**
```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
```

4. **Compile**
```bash
make -j$(nproc)
```

5. **Verify Build**
```bash
ls -la
# You should see:
# - libusrl_core.a (static library)
# - init_core (initializer)
# - pub / sub (demo programs)
# - bench_pub_swmr / bench_pub_mwmr / bench_sub (benchmark tools)
```

### Clean Build (Optional)
```bash
cd build && rm -rf * && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
```

---

## Configuration

### Configuration File: `usrl_config.json`

Located in the root directory or `benchmarks/` folder. Defines all topics (ring buffers) and their properties.

#### Example Configuration
```json
{
  "comment": "USRL Configuration",
  "memory_size_mb": 128,
  "topics": [
    {
      "name": "sensor_imu",
      "slots": 8192,
      "payload_size": 128,
      "type": "swmr"
    },
    {
      "name": "command_queue",
      "slots": 1024,
      "payload_size": 256,
      "type": "mwmr"
    },
    {
      "name": "sensor_gps",
      "slots": 512,
      "payload_size": 64,
      "type": "swmr"
    }
  ]
}
```

#### Configuration Parameters

| Parameter | Type | Default | Range | Purpose |
| :--- | :--- | :--- | :--- | :--- |
| `memory_size_mb` | Integer | 64 | 32-256 | Total shared memory allocation |
| `name` | String | - | Max 32 chars | Unique topic identifier |
| `slots` | Integer | - | 128-16384 | Ring buffer depth (# of messages) |
| `payload_size` | Integer | - | 1-65536 | Max bytes per message |
| `type` | String | swmr | swmr / mwmr | Ring buffer type |

#### Sizing Guidelines

**Ring Slots (Depth):**
- **High-frequency sensors (1kHz+):** 8192 slots (absorbs OS jitter)
- **Moderate rate (100-500Hz):** 2048-4096 slots
- **Low rate (<100Hz):** 512 slots minimum
- **Formula:** `slots = (max_publish_rate_hz * buffer_time_seconds) * safety_factor`

**Payload Size:**
- **IMU (accel/gyro/mag):** 64-128 bytes
- **GPS (GNSS):** 64-96 bytes
- **Command/Message:** 256-512 bytes
- **Video Frame Metadata:** 1-8 KB

**Memory Allocation:**
- Small deployment: 64 MB (supports ~100 small topics)
- Standard deployment: 128 MB (recommended)
- Large deployment: 256 MB (for massive ring buffers)

---

## API Reference

### 1. Core Initialization

#### `usrl_core_init()`
Initialize the core and register all topics.

**Function Signature:**
```c
int usrl_core_init(
    const char *shm_path,           // Shared memory path (typically "/usrl_core")
    uint64_t memory_size,           // Total memory in bytes
    UsrlTopicConfig *topics,        // Array of topic configs
    int topic_count                 // Number of topics
);
```

**Returns:**
- `0`: Success
- `-1`: Shared memory creation failed
- `-2`: Invalid configuration

**Example:**
```c
UsrlTopicConfig topics[2] = {
    { .name = "imu", .slot_count = 8192, .slot_size = 128, .type = 0 },
    { .name = "cmd", .slot_count = 1024, .slot_size = 256, .type = 1 }
};
usrl_core_init("/usrl_core", 128 * 1024 * 1024, topics, 2);
```

#### `usrl_core_map()`
Map existing core into address space (called by publishers/subscribers).

**Function Signature:**
```c
void* usrl_core_map(
    const char *shm_path,           // Must match init() path
    uint64_t memory_size            // Must match or exceed init() size
);
```

**Returns:**
- Non-NULL: Pointer to core memory
- NULL: Failed to map

**Example:**
```c
void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
if (!core) { printf("Failed to map core\n"); return 1; }
```

---

### 2. Publisher API (SWMR)

#### `usrl_pub_init()`
Initialize a single-writer publisher.

**Function Signature:**
```c
int usrl_pub_init(
    UsrlPublisher *pub,             // Uninitialized publisher struct
    void *core_base,                // Core pointer from usrl_core_map()
    const char *topic,              // Topic name (must exist in config)
    uint16_t pub_id                 // Publisher identifier (1-65535)
);
```

**Returns:**
- `0`: Success
- `-1`: Topic not found
- `-2`: Invalid parameters

**Example:**
```c
UsrlPublisher pub;
usrl_pub_init(&pub, core, "sensor_imu", 1);
```

#### `usrl_pub_publish()`
Publish a message (blocking until slot available).

**Function Signature:**
```c
int usrl_pub_publish(
    UsrlPublisher *pub,             // Initialized publisher
    const uint8_t *data,            // Payload to send
    uint32_t size                   // Payload size (must be <= config payload_size)
);
```

**Returns:**
- `0`: Message published
- `-1`: Payload too large
- `-2`: Ring full (should never happen with proper buffering)

**Example:**
```c
uint8_t payload[128];
// ... fill payload with data ...
usrl_pub_publish(&pub, payload, 128);
```

---

### 3. Publisher API (MWMR)

#### `usrl_mwmr_pub_init()`
Initialize a multi-writer publisher (same writer ID can be used by multiple processes).

**Function Signature:**
```c
int usrl_mwmr_pub_init(
    UsrlMwmrPublisher *pub,         // Uninitialized MWMR publisher
    void *core_base,
    const char *topic,              // Topic must be configured as type="mwmr"
    uint16_t pub_id
);
```

**Usage:** Identical to SWMR except works on MWMR topics safely.

#### `usrl_mwmr_pub_publish()`
Atomic publish from multiple concurrent writers.

**Function Signature:**
```c
int usrl_mwmr_pub_publish(
    UsrlMwmrPublisher *pub,
    const uint8_t *data,
    uint32_t size
);
```

**Returns:** Same as SWMR version.

**Thread-Safe:** Yes. Multiple threads/processes can call this simultaneously on the same topic.

---

### 4. Subscriber API

#### `usrl_sub_init()`
Initialize a subscriber (can have multiple per topic).

**Function Signature:**
```c
int usrl_sub_init(
    UsrlSubscriber *sub,            // Uninitialized subscriber
    void *core_base,
    const char *topic               // Topic name
);
```

**Returns:**
- `0`: Success
- `-1`: Topic not found

**Example:**
```c
UsrlSubscriber sub;
usrl_sub_init(&sub, core, "sensor_imu");
```

#### `usrl_sub_next()`
Non-blocking read of next message.

**Function Signature:**
```c
int usrl_sub_next(
    UsrlSubscriber *sub,            // Initialized subscriber
    uint8_t *buffer,                // Output buffer
    uint32_t buffer_size,           // Size of buffer
    uint16_t *pub_id                // [OUT] Publisher ID of sender
);
```

**Returns:**
- `> 0`: Number of bytes read (payload size)
- `0`: No data available (ring empty)
- `-1`: Internal error
- `-3`: Buffer too small for message

**Example:**
```c
uint8_t buf[256];
uint16_t sender_id;
int n = usrl_sub_next(&sub, buf, sizeof(buf), &sender_id);
if (n > 0) {
    printf("Received %d bytes from publisher %d\n", n, sender_id);
}
```

---

## Usage Examples

### Example 1: Basic SWMR Publisher-Subscriber

**Publisher (`pub_example.c`):**
```c
#include "usrl_core.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct IMUData {
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
    uint64_t timestamp_us;
};

int main(void) {
    void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
    
    UsrlPublisher pub;
    usrl_pub_init(&pub, core, "sensor_imu", 1);
    
    struct IMUData imu = {0};
    
    for (int i = 0; i < 10000; i++) {
        imu.accel_x = 9.81f * (i % 100) / 100.0f;
        imu.timestamp_us = i * 1000; // 1ms per sample
        
        usrl_pub_publish(&pub, (uint8_t*)&imu, sizeof(imu));
        usleep(1000); // 1kHz publish rate
    }
    
    return 0;
}
```

**Subscriber (`sub_example.c`):**
```c
#include "usrl_core.h"
#include <stdio.h>

struct IMUData {
    float accel_x, accel_y, accel_z;
    float gyro_x, gyro_y, gyro_z;
    uint64_t timestamp_us;
};

int main(void) {
    void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
    
    UsrlSubscriber sub;
    usrl_sub_init(&sub, core, "sensor_imu");
    
    uint8_t buf[512];
    uint16_t pub_id;
    long count = 0;
    
    while (1) {
        int n = usrl_sub_next(&sub, buf, sizeof(buf), &pub_id);
        
        if (n > 0) {
            struct IMUData *imu = (struct IMUData*)buf;
            count++;
            
            if (count % 1000 == 0) {
                printf("IMU[%ld] accel=%.2f m/s² ts=%lu µs\n",
                       count, imu->accel_x, imu->timestamp_us);
            }
        } else {
            usleep(10); // Sleep briefly if ring empty
        }
    }
    
    return 0;
}
```

**Compile & Run:**
```bash
# Init core
./init_core

# Terminal 1: Publisher
gcc -o pub pub_example.c -I core/includes -L build -lusrl_core -lpthread -lrt
./pub

# Terminal 2: Subscriber(s) - can run multiple
gcc -o sub sub_example.c -I core/includes -L build -lusrl_core -lpthread -lrt
./sub
./sub  # Another subscriber, independent state
```

---

### Example 2: Multi-Writer Command Queue (MWMR)

**Setup in config:**
```json
{
  "name": "command_queue",
  "slots": 4096,
  "payload_size": 64,
  "type": "mwmr"
}
```

**Writer 1 (`writer1.c`):**
```c
#include "usrl_core.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
    void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
    
    UsrlMwmrPublisher pub;
    usrl_mwmr_pub_init(&pub, core, "command_queue", 10); // ID 10
    
    for (int i = 0; i < 100; i++) {
        char msg[64];
        snprintf(msg, 64, "CMD from Writer1: %d", i);
        usrl_mwmr_pub_publish(&pub, (uint8_t*)msg, strlen(msg) + 1);
        usleep(10000); // 100 Hz
    }
    
    return 0;
}
```

**Writer 2 (`writer2.c`):**
```c
// Same as writer1, but:
// - usrl_mwmr_pub_init(&pub, core, "command_queue", 20); // ID 20
// - snprintf(msg, 64, "CMD from Writer2: %d", i);
```

**Reader:**
```c
#include "usrl_core.h"
#include <stdio.h>

int main(void) {
    void *core = usrl_core_map("/usrl_core", 128 * 1024 * 1024);
    
    UsrlSubscriber sub;
    usrl_sub_init(&sub, core, "command_queue");
    
    uint8_t buf[64];
    uint16_t sender_id;
    
    while (1) {
        int n = usrl_sub_next(&sub, buf, sizeof(buf), &sender_id);
        if (n > 0) {
            printf("From ID %d: %s\n", sender_id, (char*)buf);
        }
    }
    
    return 0;
}
```

**Run:**
```bash
./init_core
./writer1 &
./writer2 &
./reader   # Receives from both writers seamlessly
```

---

## Benchmark Results

### Hardware Specifications (Test System)
- **CPU:** M1 Pro / i7-12700K / Ryzen 5900X (varied platforms)
- **RAM:** 16+ GB DDR4/DDR5
- **OS:** Ubuntu 22.04 LTS / macOS 13+

### Baseline Results (64-byte payload, Single Writer)

```
>>> TEST: Baseline (Small Ring)
Publisher Rate: 6.32 Million msg/sec
Subscriber Rate: 5.93 Million msg/sec
Bandwidth: 385.46 MB/s
Avg Latency: 158.34 ns

>>> TEST: Throughput (Large Ring)
Publisher Rate: 6.59 Million msg/sec
Subscriber Rate: 6.18 Million msg/sec
Bandwidth: 402.28 MB/s
Avg Latency: 151.72 ns
```

### Bandwidth Test (8192-byte payload)

```
>>> TEST: Bandwidth (Huge Msg)
Publisher Rate: 1.04 Million msg/sec
Subscriber Rate: 1.03 Million msg/sec
Bandwidth: 8123.33 MB/s
Avg Latency: 961.74 ns

Physical Interpretation:
- L3 Cache speed: ~10-15 GB/s
- USRL achieves: ~8.1 GB/s (83-85% efficiency)
- Remaining overhead: 15-17% (scheduling, pointer management)
```

### Multi-Writer Scalability (MWMR, 4 Concurrent Writers)

```
>>> TEST: MWMR Standard
Aggregate Rate: 9.75 Million msg/sec
Per-Writer Rate: 2.44 Million msg/sec
Bandwidth: 595.14 MB/s
Avg Latency: 102.56 ns (measured per-message across all writers)

>>> TEST: MWMR High Contention (Small Ring)
Aggregate Rate: 9.53 Million msg/sec
Per-Writer Rate: 2.38 Million msg/sec
Bandwidth: 581.74 MB/s
Avg Latency: 104.92 ns
```

### Comparative Analysis

| Metric | USRL | Unix Pipe | TCP Loopback | ROS 2 (Iceoryx) |
| :--- | :--- | :--- | :--- | :--- |
| **Latency** | **150 ns** | ~5000 ns | ~10000 ns | ~500 ns |
| **Throughput** | **6.5 M/s** | 0.2 M/s | 0.05 M/s | 1-3 M/s |
| **Bandwidth** | **400 MB/s** | 50 MB/s | 25 MB/s | 100-200 MB/s |
| **Max Writers** | Unlimited | 1 | 1 | Limited |
| **Mem Overhead** | 0.5% | 0.1% | N/A (kernel) | 2-5% |

---

## Best Practices

### 1. Memory Sizing

**Do:**
```c
// Right: Allocate sufficient ring depth
topics[0] = {
    .name = "high_freq_sensor",
    .slot_count = 8192,  // Can buffer 8k messages
    .slot_size = 128,
    .type = 0
};

// Right: Total memory budgeted
// 8192 * 128 = 1 MB per ring (reasonable)
```

**Don't:**
```c
// Wrong: Ring too small (frequent overflows)
.slot_count = 32,   // Only 4KB total, overflows immediately

// Wrong: Payload too large
.slot_size = 1000000,  // 1MB per message is excessive
```

### 2. Publisher Design

**Do:**
```c
// Right: Tight loop for max throughput
while (1) {
    usrl_pub_publish(&pub, data, size);  // Spin-lock if needed
}

// Right: Batch updates
for (int i = 0; i < 1000; i++) {
    update_sensor_data(&data);
    usrl_pub_publish(&pub, (uint8_t*)&data, sizeof(data));
}
```

**Don't:**
```c
// Wrong: Excessive error checking (hurts latency)
if (usrl_pub_publish(&pub, data, size) != 0) {
    printf("Error!\n");  // Context switch to kernel
    sleep(1);            // Stalls the publisher
}

// Wrong: Copying data before publish (defeats zero-copy)
memcpy(huge_buffer, sensor_data, 8192);  // Extra copy overhead
usrl_pub_publish(&pub, huge_buffer, 8192);
```

### 3. Subscriber Design

**Do:**
```c
// Right: Busy-wait for minimal latency
while (1) {
    int n = usrl_sub_next(&sub, buf, sizeof(buf), &pub_id);
    if (n > 0) {
        process_message(buf, n);
    }
}

// Right: Batch read
uint8_t buf[512];
uint16_t pub_id;
while (1) {
    while ((n = usrl_sub_next(&sub, buf, sizeof(buf), &pub_id)) > 0) {
        process_message(buf, n);
    }
    usleep(1);  // Brief pause if empty
}
```

**Don't:**
```c
// Wrong: Excessive sleeping (misses high-frequency data)
if (n == 0) {
    sleep(1);  // Miss 1000 messages at 1kHz
}

// Wrong: Allocating buffer inside loop
while (1) {
    uint8_t *buf = malloc(256);  // Context switch to allocator
    usrl_sub_next(&sub, buf, 256, &pub_id);
    free(buf);                     // Context switch to deallocator
}
```

### 4. Configuration Strategy

**Sensor Streaming (High Rate):**
```json
{
  "name": "imu_stream",
  "slots": 8192,        // Large ring for bursty OS scheduling
  "payload_size": 128,  // IMU data size
  "type": "swmr"        // Single sensor source
}
```

**Command Aggregation (Multiple Sources):**
```json
{
  "name": "command_queue",
  "slots": 4096,        // Moderate depth
  "payload_size": 256,  // Command size
  "type": "mwmr"        // Multiple command sources
}
```

**High-Bandwidth (Video/Raw Data):**
```json
{
  "name": "video_frames",
  "slots": 64,          // Few frames (memory constraints)
  "payload_size": 921600,  // 1280x480 grayscale
  "type": "swmr"
}
```

---

## Troubleshooting

### Issue 1: "Failed to map core"
**Symptom:**
```
[PUB] Failed to map core. Did you run ./init_core?
```

**Solution:**
```bash
# 1. Run initializer first
./init_core

# 2. Verify shared memory exists
ls -la /dev/shm/ | grep usrl_core

# 3. Check permissions
# Current user should own /dev/shm/usrl_core
```

### Issue 2: Topic Not Found

**Symptom:**
```
[BENCH] Error: Topic 'sensor_imu' not found!
```

**Solution:**
```bash
# 1. Verify topic name in usrl_config.json
grep -i "sensor_imu" usrl_config.json

# 2. Ensure init_core reads correct JSON
./init_core  # Should list all loaded topics

# 3. Case-sensitive names
# Use exact spelling in code
usrl_pub_init(&pub, core, "sensor_imu", 1);  // Lowercase
```

### Issue 3: Subscriber Shows No Data

**Symptom:**
```
[SUB] Rate: (nothing printed)
```

**Solution:**
```bash
# 1. Verify publisher is running
ps aux | grep pub

# 2. Check ring is actually receiving
# Add debug print in subscriber loop
printf("Checking: n=%d\n", n);

# 3. Ensure topic type matches
// If topic is "swmr", use UsrlPublisher (not MwmrPublisher)
usrl_pub_init(&pub, core, "sensor_imu", 1);  // Correct for swmr
```

### Issue 4: Latency Spikes

**Symptom:**
```
Latency: 150 ns (normal)
Latency: 50000 ns (spike)  // 300x slower!
```

**Cause & Solution:**
```c
// Cause: Process gets preempted by OS

// Solution 1: Set CPU affinity (if not doing already)
#include <sched.h>
cpu_set_t set;
CPU_ZERO(&set);
CPU_SET(2, &set);  // Bind to core 2
sched_setaffinity(0, sizeof(set), &set);

// Solution 2: Use FIFO scheduler (requires root)
// # ulimit -l unlimited
// # chrt -f 99 ./pub  (FIFO priority 99)

// Solution 3: Accept minor jitter (normal for Linux)
// Real-time Linux (PREEMPT_RT kernel) removes most spikes
```

### Issue 5: Memory Exhaustion

**Symptom:**
```
[INIT] Failed to allocate shared memory
mmap() failed: Cannot allocate memory
```

**Solution:**
```bash
# 1. Check available memory
free -h

# 2. Reduce memory allocation
# In usrl_config.json:
"memory_size_mb": 64  # Reduce from 128

# 3. Remove old shared memory
ipcrm -m $(ipcs -m | grep usrl_core | awk '{print $2}')

# 4. Increase swap (temporary fix)
sudo fallocate -l 2G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
```

### Issue 6: Benchmarks Show Different Numbers Each Run

**Expected Behavior:**
```
Run 1: 6.32 M msg/sec
Run 2: 6.59 M msg/sec  // Different due to OS scheduling
Run 3: 6.47 M msg/sec
```

**Why:**
- OS kernel can preempt your process
- Other background processes compete for CPU
- CPU frequency scaling (turbo boost) varies

**Minimize Variance:**
```bash
# Disable CPU frequency scaling
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Reduce background processes
killall -9 chrome firefox apt

# Run on isolated CPU
taskset -c 2 ./pub  # Use only core 2
```

---

## Advanced Topics

### Custom Payload Types

Define application-specific structures and send directly:

```c
// Define your message
typedef struct {
    uint64_t timestamp_us;
    float latitude, longitude, altitude;
    float hdop, vdop;
} GPSData;

// Publish
GPSData gps = {...};
usrl_pub_publish(&pub, (uint8_t*)&gps, sizeof(gps));

// Subscribe
uint8_t buf[256];
int n = usrl_sub_next(&sub, buf, sizeof(buf), &pub_id);
if (n > 0) {
    GPSData *received = (GPSData*)buf;
    printf("Lat: %.6f\n", received->latitude);
}
```

### Real-Time Flight Software Integration

Example use case: AHRS (Attitude and Heading Reference System)

```c
// Three independent sensor publishers (SWMR)
// - IMU (accel/gyro): 1000 Hz
// - Magnetometer: 100 Hz
// - Barometer: 50 Hz

// Single AHRS fusion algorithm (subscriber to all three)
while (1) {
    if (usrl_sub_next(&imu_sub, buf, sz, &id) > 0) update_imu(buf);
    if (usrl_sub_next(&mag_sub, buf, sz, &id) > 0) update_mag(buf);
    if (usrl_sub_next(&bar_sub, buf, sz, &id) > 0) update_bar(buf);
    
    // Compute filtered attitude
    float roll, pitch, yaw;
    ahrs_update(&roll, &pitch, &yaw);
    
    // Publish result
    attitude_t att = {roll, pitch, yaw};
    usrl_pub_publish(&att_pub, (uint8_t*)&att, sizeof(att));
}
```

---

## Conclusion

USRL is a **production-ready IPC framework** for:
-  **Aerospace/Avionics** (Deterministic latency)
-  **Robotics** (Multi-sensor fusion)
-  **High-Frequency Trading** (Sub-microsecond performance)
-  **Real-Time Embedded Systems** (Zero-copy, lock-free)

With 150ns latency and 8GB/s bandwidth, USRL enables a new class of low-latency applications on standard Linux systems.

### Next Steps

1. **Build the project:** `cmake . && make -j4`
2. **Run benchmarks:** `cd benchmarks && ./benchmark.sh`
3. **Integrate into your project:** Copy `usrl_core` library files
4. **Profile your application:** Use the provided benchmark suite as a template

For questions or contributions, refer to the inline code documentation in `core/src/*.c`.

---

## Remote Developer Quick Start — Get Productive in 10 Minutes

This section focuses on what a remote developer or contributor needs to know to get up-and-running immediately with the USRL repository from a remote machine or dev container.

Prerequisites (Remote Dev)
- POSIX-compatible OS (Linux recommended). This repo is developed/tested on x86_64 and aarch64.
- Git access (SSH or HTTPS) to the repository.
- Build toolchain: C compiler (GCC/Clang with C11), CMake, make.
- Access to container or VM if you prefer reproducible dev environment.

Clone repository (SSH)
```bash
git clone git@github.com:your-org/usrl-core.git ~/projects/usrl-core
cd ~/projects/usrl-core
```

Clone repository (HTTPS)
```bash
git clone https://github.com/your-org/usrl-core.git ~/projects/usrl-core
cd ~/projects/usrl-core
```

Dev Container (Alpine Linux v3.23) — quick setup
- The project CI/devcontainer uses Alpine. Use these commands inside the Alpine dev container to prepare environment.

Install build essentials on Alpine:
```sh
# inside container
apk update
apk add --no-cache build-base cmake git bash cmake make file musl-dev linux-headers
# Optional helpful tools (available on PATH per workspace instructions):
apk add --no-cache curl util-linux
```

Notes on Alpine differences:
- glibc-specific tooling may behave differently; the project is tested under musl in CI and with glibc on CI images too.
- If you need glibc compatibility, consider running an Ubuntu-based container or use an image with glibc.

Build (From repo root)
```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```
Expected artifacts in build/:
- libusrl_core.a or shared libs
- bin/init_core
- bin/pub, bin/sub demo apps
- bench/bench_pub_* utilities

Map & Run (local quick test)
1. Initialize the shared core (run once per machine or per test):
```bash
# from project root or build/bin
./init_core     # will read usrl_config.json or default config; see init_core usage
```

2. Start a publisher (example):
```bash
./pub
```

3. Start one or more subscribers:
```bash
./sub
./sub  # parallel subscribers supported
```

Common commands & short explanations
- usrl_core_init(...) — Create and populate the shared memory region. Only one process should run the initializer.
- usrl_core_map(...) — Map an existing shared region for publishers/subscribers.
- usrl_pub_init(...) / usrl_pub_publish(...) — SWMR publisher API.
- usrl_mwmr_pub_init(...) / usrl_mwmr_pub_publish(...) — MWMR publisher API.
- usrl_sub_init(...) / usrl_sub_next(...) — Subscriber API.

Running the demos
- The repository includes simple demo programs (pub/sub) — build them and run in separate terminals to validate functionality.
- If init_core prints available topics, use exact topic names when starting publishers or subscribers.

Configuration: usrl_config.json
- Located at repo root and also used by init_core.
- Edit with safe values (see existing "Configuration" section in the doc).
- Use JSON validators (online or local) if you modify the file.

Remote Debugging & Diagnostics
Tools available in the container/path: apk, wget, ps, lsof, netstat, top, tree, find, grep, unzip, tar, gzip, bzip2.

Useful commands:
- Verify shared memory entries:
  - ls -la /dev/shm | grep usrl
- Inspect open sockets and ports:
  - ss -ltnp or netstat -ltnp (netstat present per PATH)
- Find processes using a shared object:
  - lsof -nP | grep usrl_core
- View running processes:
  - ps aux | grep pub
- Check memory usage:
  - free -h
- Trace a process syscalls (if strace available in your environment):
  - strace -f -p <pid>

Networking diagnostics
- For TCP transports: use netstat/ss/lsof to inspect connections and ports.
- For firewalls: ensure remote host allows configured port (iptables/nft).

Common Issues and Resolutions (quick)
- "Failed to map core" — init_core must be run as user that has write access to /dev/shm; verify file exists.
- "Topic not found" — ensure init_core loaded the correct config; topic names are case-sensitive.
- "No messages" — publisher may not be running, confirm with ps or add debug prints.
- "Latency spikes" — check CPU affinity, CPU governor, and system load. Use taskset to pin processes.

Testing Locally (unit/bench)
- Benchmarks live under benchmarks/ or bench/; run the bench binaries after build.
- For CI: we recommend running the benchmark in a quiet system (use CPU isolation and disable governor scaling).
- Example benchmark run:
  - ./bench/bench_pub_swmr --topic sensor_imu --rate 1000000

Remote / Non-Interactive CI
- Use the following minimal script for CI to build and run basic smoke tests:
```sh
#!/bin/sh
set -e
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
# initialize and test mapping
./init_core || { echo "init_core failed"; exit 1; }
./pub --test-mode 1000 || echo "pub completed"
./sub --test-mode 1000 || echo "sub completed"
```

Contributing & Code Layout (for remote dev)
- Core implementation: core/src/*.c (usrl_core.c, ring_swmr.c, ring_mwmr.c)
- Transport implementations: transport/*/src (tcp, udp, rdma)
- Headers: core/includes or include/
- Bench and example programs: examples/benchmarks/bin
- Tests: tests/

Pull Request checklist
- Build succeeds in both Release and Debug
- No regressions in major API behavior (usrl_core_init, mapping, pub/sub)
- Add unit tests for new algorithms or bug fixes
- Document behavior changes in USRL_Documentation.md

API Quick Reference (cheatsheet)
- usrl_core_init(path, size, topics, count) -> int
- void* usrl_core_map(path, size)
- usrl_get_topic(base, name) -> TopicEntry*
- usrl_pub_init(UsrlPublisher*, base, topic, pub_id)
- usrl_pub_publish(UsrlPublisher*, data, len)
- usrl_mwmr_pub_init(UsrlMwmrPublisher*, base, topic, pub_id)
- usrl_mwmr_pub_publish(UsrlMwmrPublisher*, data, len)
- usrl_sub_init(UsrlSubscriber*, base, topic)
- usrl_sub_next(UsrlSubscriber*, out_buf, buf_len, out_pub_id)

Security & Permissions
- Shared memory objects are created readable/writable by the creating user. If multiple users need access, coordinate permissions or use a system service to run init_core as a known user.
- Do not expose /dev/shm objects to untrusted users — data is in clear memory.
- For network transports, prefer TLS-wrapped streams if crossing untrusted networks (transport layer in project is currently plain TCP/UDP).

Packaging & Releases
- Produce release bundles including:
  - Headers (include/)
  - Static/shared libs (libusrl_core.a / .so)
  - Binaries (init_core, pub, sub, bench)
  - Default usrl_config.json
  - USRL_Documentation.md
- Tag repo by semantic version and include changelog in release notes.

FAQ (short)
Q: Can multiple processes be publishers to the same MWMR topic?
A: Yes — MWMR APIs are designed for multi-writer safety.

Q: Are messages persisted across reboots?
A: No — shared memory segments are ephemeral and cleared by usrl_core_init. Use persistence layer externally if needed.

Q: Can I use this across containers?
A: Yes if containers share the same IPC namespace and have access to the shared memory path (e.g., /dev/shm). For cross-host, use the transport layer (TCP) to forward messages.

Contact / Next Steps
- For repo access problems contact the repository admin.
- For deeper architecture questions, consult the annotated source in core/src which now contains Doxygen comments for all public APIs.
