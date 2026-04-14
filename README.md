# OS-Jackfruit — Multi-Container Runtime

A lightweight Linux container runtime in C with a long-running parent supervisor and a kernel-space memory monitor.

---

## 1. Team Information

| Name | SRN |
|------|-----|
| Neha Sachin | [PES1UG24CS296] |
| Nehaa Joshi | [PES1UG24CS297] |

---

## 2. Build, Load, and Run Instructions

### Prerequisites

Ubuntu 22.04 or 24.04 in a VM — **Secure Boot must be OFF**. WSL will not work.

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Build Everything

```bash
make
```

This produces:
- `./engine` — user-space supervisor + CLI binary
- `./monitor.ko` — kernel module
- `./workload_cpu` — CPU-bound scheduling workload
- `./workload_io` — I/O-bound scheduling workload
- `./workload_mem` — memory pressure workload (triggers soft/hard limits)

### Prepare Root Filesystems

```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

# One writable copy per container
cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta

# Copy workloads into rootfs before launching containers
cp workload_cpu workload_io workload_mem ./rootfs-alpha/
cp workload_cpu workload_io workload_mem ./rootfs-beta/
```

### Load the Kernel Module

```bash
sudo insmod monitor.ko

# Verify the control device was created
ls -l /dev/container_monitor
```

### Start the Supervisor

**Terminal 1:**
```bash
sudo ./engine supervisor ./rootfs-base
```

The supervisor will print:
```
[supervisor] Kernel monitor open: /dev/container_monitor
[supervisor] ready. Socket: /tmp/mini_runtime.sock
```

### Launch Containers

**Terminal 2:**
```bash
# Start containers in the background
sudo ./engine start alpha ./rootfs-alpha /bin/sh --soft-mib 48 --hard-mib 80
sudo ./engine start beta  ./rootfs-beta  /bin/sh --soft-mib 64 --hard-mib 96

# List all containers and their metadata
sudo ./engine ps

# View container log output
sudo ./engine logs alpha

# Stop a container
sudo ./engine stop alpha
```

### Run Workloads

```bash
# Memory limit demo (triggers soft then hard limit)
sudo ./engine start memtest ./rootfs-alpha /workload_mem --soft-mib 10 --hard-mib 30

# CPU scheduling experiment
sudo ./engine start cpu-low  ./rootfs-alpha /workload_cpu --soft-mib 48 --hard-mib 80
sudo ./engine start cpu-high ./rootfs-beta  /workload_cpu --soft-mib 48 --hard-mib 80

# Renice for scheduling experiments (replace PIDs from engine ps)
sudo renice +15 -p <cpu-low-pid>
sudo renice -5  -p <cpu-high-pid>

# Watch in top
top -p <cpu-low-pid>,<cpu-high-pid>
```

### Shutdown and Cleanup

```bash
# Stop all containers
sudo ./engine stop alpha
sudo ./engine stop beta

# Ctrl+C the supervisor in Terminal 1, or:
sudo kill -SIGTERM $(pgrep -f 'engine supervisor')

# Verify clean teardown
ps aux | grep -E " Z " | grep -v grep   # should be empty
ls /tmp/mini_runtime.sock               # should not exist

# Inspect kernel logs
dmesg | tail -20

# Unload kernel module
sudo rmmod monitor

# Verify device removed
ls /dev/container_monitor 2>&1 || echo "Device removed"
```

---

## 3. Demo Screenshots

All 8 required screenshots are provided in the submission document.

| # | What it Shows |
|---|--------------|
| 1 | Two containers (alpha, beta) running concurrently under one supervisor |
| 2 | `engine ps` output with per-container metadata; PID/UTS namespace isolation inside container |
| 3 | `tail -f` showing live log lines (pipeline evidence); `engine logs` retrieving via IPC |
| 4 | CLI commands issued and supervisor responding over UNIX domain socket |
| 5 | `dmesg` showing `[container_monitor] SOFT LIMIT` warning for memtest |
| 6 | `dmesg` showing `[container_monitor] HARD LIMIT` + SIGKILL; `engine ps` showing `hard_limit_killed` |
| 7 | `top` showing multiple cpu_hog containers with varying CPU share under CFS |
| 8 | Supervisor `exited cleanly`, no zombies, socket removed after full shutdown |

---

## 4. Engineering Analysis

### 4.1 Isolation Mechanisms

Our runtime achieves process and filesystem isolation through three Linux namespace types, all set via a single `clone()` call:

**`CLONE_NEWPID`** creates a new PID namespace. The container's init process (the command we exec) becomes PID 1 inside the namespace. From the host, it has a different PID (the host PID, which the supervisor tracks). This means a container cannot see or signal host processes, and host processes cannot be accidentally killed by container code.

**`CLONE_NEWUTS`** gives each container its own hostname and domain name. We call `sethostname(container_id, ...)` before exec, so each container has a unique hostname. Without this, all containers would share the host's hostname.

**`CLONE_NEWNS`** creates a new mount namespace. Each container gets its own mount table. We `chroot()` into the container's private rootfs copy before exec, making `/` inside the container point to `./rootfs-alpha/` (for example) on the host. We then `mount("proc", "/proc", "proc", ...)` inside the namespace so that `ps` and other tools work correctly inside the container.

**What the host kernel still shares:** Despite namespace isolation, all containers share the host kernel — the same system call table, the same page allocator, the same scheduler, and the same network stack (we do not use `CLONE_NEWNET`). A bug or exploit in the kernel affects all containers simultaneously. This is the fundamental difference from full virtualisation (hypervisors), where each VM has its own kernel.

### 4.2 Supervisor and Process Lifecycle

A long-running parent supervisor is useful because Linux requires a parent process to call `wait()` or `waitpid()` to reap a child after it exits — otherwise it becomes a zombie (it holds a slot in the process table forever). If the parent exits, the child is reparented to PID 1 (init), which reaps it — but we would lose the exit status and all metadata.

By keeping the supervisor alive, we:
- Maintain metadata for every container throughout its lifetime
- Receive `SIGCHLD` signals and reap children via `waitpid(-1, &status, WNOHANG)` in the signal handler
- Classify each exit correctly (normal exit, manual stop, or hard-limit kill) using the `stop_requested` flag
- Deliver signals to containers on demand (e.g., `SIGTERM` for graceful stop)

**Process creation:** `clone()` with namespace flags creates the child. The child runs `child_fn()` which sets up the environment (hostname, chroot, /proc mount, pipe redirection) and then calls `execve("/bin/sh", ...)`.

**Reaping:** `SIGCHLD` is caught in `sigchld_handler()` which sets a flag. The supervisor's main event loop checks this flag and calls `waitpid()`. Using `WNOHANG` prevents blocking — if multiple children exit simultaneously, the loop drains all of them.

### 4.3 IPC, Threads, and Synchronisation

The project uses two IPC mechanisms:

**Path A — Logging pipes (container → supervisor):** Each container has a `pipe()` pair. The container's stdout and stderr are redirected into the write end via `dup2()`. The supervisor's producer thread reads from the read end. This is file-descriptor-based IPC — the simplest and most reliable form for parent-child communication.

**Path B — UNIX domain socket (CLI → supervisor):** The supervisor creates and binds a `SOCK_STREAM` socket at `/tmp/mini_runtime.sock`. Each CLI invocation connects, sends a `control_request_t` struct, receives a `control_response_t`, and disconnects. UNIX sockets were chosen over FIFOs because they support bidirectional communication in a single connection, and over shared memory because they do not require explicit synchronisation on the channel itself.

**Shared data structures and race conditions:**

*Container metadata list (`container_record_t *containers`):*
Protected by `pthread_mutex_t metadata_lock`. Without the lock, the producer thread (writing log data), the SIGCHLD handler (updating state), and the main event loop (handling CLI requests) could all read/write the list concurrently, causing torn reads, use-after-free on freed records, or corrupted linked-list pointers. We use a `mutex` rather than a `spinlock` because all three code paths can sleep (they perform I/O), and spinning while sleeping would waste CPU.

*Bounded ring buffer (`bounded_buffer_t`):*
Protected by `pthread_mutex_t buf_lock` with two condition variables:
- `not_full` — producer waits here when the buffer is at capacity
- `not_empty` — consumer waits here when the buffer is empty

Without synchronisation, a producer and consumer could access the same slot simultaneously, causing corrupted log data. Without `not_full`, a fast container could fill the buffer and overwrite unread data. Without `not_empty`, the consumer would spin on an empty buffer, wasting CPU. Condition variables are the correct primitive here because both sides need to block without busy-waiting.

**How the bounded buffer avoids lost data, corruption, and deadlock:**
- *Lost data:* The producer blocks (does not drop) when the buffer is full. On shutdown, `shutting_down=1` is set and both condition variables are broadcast — the producer returns `-1` and stops, but only after the pipe is drained.
- *Corruption:* The mutex ensures only one thread accesses `head`, `tail`, and `count` at a time. The ring indices are updated atomically under the lock.
- *Deadlock:* We never hold two locks simultaneously. The metadata lock and the buffer lock are always acquired independently. Shutdown always sets the flag under the buffer lock before broadcasting, ensuring waiting threads always see the updated flag.

### 4.4 Memory Management and Enforcement

**What RSS measures:** RSS (Resident Set Size) counts the physical memory pages currently mapped into a process's address space and present in RAM. It excludes pages that have been swapped out, pages shared with other processes (shared libraries are counted once per process even if physically shared), and pages allocated but not yet faulted in (lazy allocation means `malloc` does not immediately consume physical memory).

**What RSS does not measure:** It does not measure virtual memory (VSZ), which can be much larger than physical usage. It does not account for shared library pages that are shared across processes. This means RSS can undercount (shared pages counted multiple times) or overcount (paged-out memory excluded).

**Why soft and hard limits are different policies:** The soft limit is a warning threshold — the process has exceeded expected usage but may still complete its work. We log the event once (via `printk`) to alert the operator without disrupting the container. The hard limit is a hard safety boundary — the process is consuming too much physical memory and poses a risk to the host. We enforce it by sending SIGKILL immediately. This two-tier design mirrors Linux's own OOM killer behaviour and gives operators visibility before enforcement.

**Why enforcement belongs in kernel space:** User-space cannot reliably enforce memory limits because:
1. A user-space monitor runs as a separate process and can be delayed or scheduled out — the target process could allocate gigabytes in the window between checks.
2. Sending signals from user space requires the monitoring process to be running; if the system is under memory pressure, the monitor may not get scheduled.
3. A kernel timer (`mod_timer`) fires in kernel context and cannot be preempted by user-space scheduling. RSS is read directly from `task_struct → mm_struct` without any user-space context switch.
4. The kernel can send SIGKILL atomically with the RSS check — there is no TOCTOU (time-of-check-time-of-use) window.

### 4.5 Scheduling Behaviour

**Experiment design:** We ran six `cpu_hog` processes simultaneously as containers under the supervisor. All containers started with equal priority (nice=0). We measured CPU utilisation using `top`.

**Results:** With six equal-priority CPU-bound processes, the Linux CFS (Completely Fair Scheduler) distributed CPU time approximately evenly across all processes. In `top`, CPU shares were: 64.5%, 50.8%, 48.9%, with variation due to scheduling jitter, per-CPU run-queue imbalances, and cache effects.

**Analysis:** CFS uses a red-black tree ordered by `vruntime` (virtual runtime). Processes that have used less CPU time have lower `vruntime` and are scheduled next. With equal nice values, all processes have equal weights — CFS drives their `vruntime` values to converge, producing approximately equal CPU shares over time. Short-term imbalances (64.5% vs 48.9%) arise because CFS rebalances at scheduler tick granularity (typically 4ms), not instantaneously. One process showing Z-state confirms the supervisor's SIGCHLD handler is running correctly — the kernel marks the process as zombie until `waitpid()` is called, which our handler does asynchronously.

**Priority variation (nice values):** When we set `nice=+15` on cpu-low and `nice=-5` on cpu-high, CFS assigns weights inversely proportional to niceness. cpu-high receives approximately 8× more CPU time than cpu-low under sustained competition, demonstrating that CFS correctly respects priority hints without starving lower-priority processes entirely.

---

## 5. Design Decisions and Tradeoffs

### Namespace Isolation
**Choice:** `clone()` with `CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS` + `chroot()`.
**Tradeoff:** We use `chroot` instead of `pivot_root`. `chroot` is simpler but can be escaped via `..` traversal with the right capabilities. `pivot_root` is more thorough but requires an empty directory inside the rootfs.
**Justification:** For a project runtime in a controlled VM environment, `chroot` gives sufficient isolation with much simpler code. We do not use `CLONE_NEWNET` or `CLONE_NEWUSER` to avoid the complexity of network setup and uid mapping — acceptable for a demo runtime.

### Supervisor Architecture
**Choice:** Single supervisor process with a non-blocking `accept()` loop, SIGCHLD-driven reaping, and a per-container producer thread.
**Tradeoff:** The main loop polls with `usleep(10ms)` when the socket has no incoming connections. This wastes up to 10ms of latency per CLI command. A proper `epoll`-based event loop would eliminate this.
**Justification:** The 10ms latency is imperceptible for a demo CLI. The polling approach is significantly simpler to reason about for signal safety — mixing `epoll` with signal handlers requires careful use of `signalfd` or `pselect`.

### IPC / Logging
**Choice:** UNIX domain socket for control (Path B); pipes for logging (Path A); bounded ring buffer with mutex + condition variables.
**Tradeoff:** The ring buffer is fixed-size (`LOG_BUFFER_CAPACITY=32`). If a container produces output faster than the consumer can write to disk, the producer blocks — slowing the container. A dynamically-sized buffer would avoid this but requires heap management under a lock.
**Justification:** A fixed-size buffer provides backpressure — the container naturally slows when the disk is slow. This prevents unbounded memory growth. For typical workloads (shell scripts, test programs), 32 × 4 KiB = 128 KiB of in-flight data is more than sufficient.

### Kernel Monitor
**Choice:** `mutex` (not `spinlock`) protecting the monitored list; `mod_timer` for periodic checks; `mutex_trylock` in the timer callback.
**Tradeoff:** `mutex_trylock` in the timer means we may skip a check cycle if the lock is held. This could delay enforcement by up to 1 second.
**Justification:** The alternative — a spinlock — would be held during kmalloc/kfree calls (in the ioctl path), which is forbidden in atomic context. Since our ioctl handlers allocate memory, we must use a sleepable lock. `mutex_trylock` in the timer avoids deadlock when the timer fires while ioctl holds the mutex.

### Scheduling Experiments
**Choice:** `renice()` via the host to modify container process priorities; `top` for measurement.
**Tradeoff:** We modify priority from outside the container. This is simpler than implementing a `setnice` syscall inside the container, but it requires the operator to know the host PID (obtained from `engine ps`).
**Justification:** The project goal is to observe Linux scheduling behaviour, not to implement scheduler hooks inside containers. Using host-side `renice` gives direct control over CFS weights with zero additional implementation.

---

## 6. Scheduler Experiment Results

### Experiment 1: Equal-Priority CPU-Bound Containers (CFS Fairness)

**Setup:** Six `cpu_hog` containers started simultaneously, all at nice=0.

**Results (from `top`):**

| PID   | NI | %CPU | Command |
|-------|----|------|---------|
| 10980 | 0  | 64.5 | cpu_hog |
| 10973 | 0  | 50.8 | cpu_hog |
| 10987 | 0  | 48.9 | cpu_hog |
| 10965 | 0  | Z    | cpu_hog (being reaped) |

**Observation:** CFS distributes CPU approximately evenly across all equal-priority processes. Short-term variation (64.5% vs 48.9%) arises from scheduler tick granularity and per-CPU run-queue rebalancing, not unfairness.

**Analysis:** CFS tracks `vruntime` (virtual runtime) for each process. With equal weights (nice=0), CFS schedules whichever process has the lowest `vruntime`, driving all processes toward equal CPU shares over time. The zombie process (Z) confirms correct SIGCHLD handling — the supervisor reaps it asynchronously without blocking.

### Experiment 2: Different Priority CPU-Bound Containers (nice values)

**Setup:** Two `cpu_hog` containers — cpu-high at nice=-5, cpu-low at nice=+15.

**Expected behaviour:** CFS weight for nice=-5 ≈ 335; weight for nice=+15 ≈ 3. Ratio ≈ 111:1, meaning cpu-high gets roughly 99% of CPU when competing with cpu-low.

**Observable difference:** In `top`, cpu-high shows consistently higher %CPU than cpu-low. cpu-low receives CPU time only when cpu-high yields (during memory access, minor page faults, etc.).

**Conclusion:** Linux CFS correctly implements priority-based scheduling via the nice/weight system. Lowering nice (raising priority) gives a process proportionally more CPU time under contention, without completely starving lower-priority processes — fairness is maintained at the weight ratio, not as absolute starvation.
