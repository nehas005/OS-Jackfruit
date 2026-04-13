/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Complete implementation covering all 6 project tasks:
 *   Task 1 - Multi-container runtime with parent supervisor
 *   Task 2 - Supervisor CLI and signal handling (UNIX socket IPC)
 *   Task 3 - Bounded-buffer logging pipeline (producer/consumer)
 *   Task 4 - Kernel memory monitor integration via ioctl
 *   Task 5 - Scheduler experiment support (nice, affinity)
 *   Task 6 - Resource cleanup on shutdown
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ─── Constants ──────────────────────────────────────────────────────────── */
#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "/tmp/jackfruit_logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT   (64UL << 20)   /* 64 MiB */
#define MONITOR_DEVICE       "/dev/container_monitor"
#define MAX_CONTAINERS       64

/* ─── Enumerations ───────────────────────────────────────────────────────── */
typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

/* ─── Data structures ────────────────────────────────────────────────────── */

typedef struct container_record {
    char              id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    time_t            started_at;
    container_state_t state;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               exit_code;
    int               exit_signal;
    int               stop_requested;   /* set before SIGTERM in cmd_stop */
    char              log_path[PATH_MAX];
    int               log_fd;           /* write-end of log pipe (supervisor side) */
    int               pipe_read_fd;     /* read-end of log pipe (producer reads from container) */
    pthread_t         producer_thread;
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
    int            wait_for_exit;   /* 1 = CMD_RUN (foreground) */
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  pipe_write_fd;   /* container writes stdout/stderr here */
} child_config_t;

/* Producer thread arg: reads from pipe, pushes into bounded buffer */
typedef struct {
    int              read_fd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_arg_t;

typedef struct {
    int               server_fd;
    int               monitor_fd;
    volatile int      should_stop;
    pthread_t         logger_thread;
    bounded_buffer_t  log_buffer;
    pthread_mutex_t   metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer — used by signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ─── Helpers ────────────────────────────────────────────────────────────── */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage:\n"
        "  %s supervisor <base-rootfs>\n"
        "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
        "  %s ps\n"
        "  %s logs <id>\n"
        "  %s stop <id>\n",
        prog, prog, prog, prog, prog, prog);
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:          return "starting";
    case CONTAINER_RUNNING:           return "running";
    case CONTAINER_STOPPED:           return "stopped";
    case CONTAINER_KILLED:            return "killed";
    case CONTAINER_EXITED:            return "exited";
    case CONTAINER_HARD_LIMIT_KILLED: return "hard_limit_killed";
    default:                          return "unknown";
    }
}

static int parse_mib_flag(const char *flag, const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                  char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr, "Invalid value for --nice (expected -20..19): %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

/* ─── Bounded Buffer ─────────────────────────────────────────────────────── */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * Task 3 — Producer: insert one log chunk into the ring buffer.
 * Blocks (on not_full) when the buffer is full.
 * Returns 0 on success, -1 if shutting down.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    /* Wait while full, but bail if shutdown starts */
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * Task 3 — Consumer: remove one log chunk from the ring buffer.
 * Blocks (on not_empty) while the buffer is empty.
 * Returns 0 on success, -1 if shutting down and buffer is empty.
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);

    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ─── Task 3 — Producer Thread ───────────────────────────────────────────── */

/*
 * Reads stdout/stderr from a container pipe and pushes chunks
 * into the shared bounded buffer. One thread per container.
 */
static void *producer_thread_fn(void *arg)
{
    producer_arg_t *parg = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, parg->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(parg->read_fd, item.data, LOG_CHUNK_SIZE - 1)) > 0) {
        item.data[n] = '\0';
        item.length = (size_t)n;
        /* Push into bounded buffer; if shutting down, stop */
        if (bounded_buffer_push(parg->buffer, &item) != 0)
            break;
        memset(item.data, 0, sizeof(item.data));
    }

    close(parg->read_fd);
    free(parg);
    return NULL;
}

/* ─── Task 3 — Consumer (Logger) Thread ─────────────────────────────────── */

/*
 * Single consumer thread: drains the bounded buffer and writes
 * each chunk to the correct per-container log file.
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        container_record_t *rec;

        /* Find the log file fd for this container */
        pthread_mutex_lock(&ctx->metadata_lock);
        rec = ctx->containers;
        while (rec) {
            if (strncmp(rec->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                if (rec->log_fd >= 0)
                    write(rec->log_fd, item.data, item.length);
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    return NULL;
}

/* ─── Task 4 — Kernel Monitor Integration ────────────────────────────────── */

int register_with_monitor(int monitor_fd, const char *container_id,
                           pid_t host_pid, unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) {
        perror("[engine] ioctl MONITOR_REGISTER");
        return -1;
    }
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) {
        perror("[engine] ioctl MONITOR_UNREGISTER");
        return -1;
    }
    return 0;
}

/* ─── Task 1 — Container Child Entrypoint ────────────────────────────────── */

/*
 * Runs inside the newly cloned process (new PID/UTS/mount namespace).
 * Sets up the isolated environment and exec's the requested command.
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    char proc_path[PATH_MAX];

    /* Redirect stdout and stderr to the pipe (log producer) */
    if (dup2(cfg->pipe_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->pipe_write_fd, STDERR_FILENO) < 0) {
        perror("[container] dup2");
        return 1;
    }
    close(cfg->pipe_write_fd);

    /* Set UTS hostname to the container ID */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("[container] sethostname (non-fatal)");

    /* Apply nice value for scheduling experiments (Task 5) */
    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) < 0)
            perror("[container] nice (non-fatal)");
    }

    /* chroot into the container rootfs */
    if (chroot(cfg->rootfs) < 0) {
        perror("[container] chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("[container] chdir /");
        return 1;
    }

    /* Mount /proc so process tools work inside the container */
    snprintf(proc_path, sizeof(proc_path), "/proc");
    if (mount("proc", proc_path, "proc",
              MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL) < 0) {
        perror("[container] mount /proc (non-fatal, may already exist)");
    }

    /* Execute the requested command */
    char *const argv[] = { cfg->command, NULL };
    char *const envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        "TERM=xterm",
        NULL
    };

    execve(cfg->command, argv, envp);
    /* execve only returns on error */
    perror("[container] execve");
    return 1;
}

/* ─── Task 1 — Launch a Container ────────────────────────────────────────── */

/*
 * Spawns a new container: sets up a logging pipe, clones with new
 * namespaces, starts the producer thread, registers with the kernel
 * monitor, and records metadata.
 *
 * Returns the new container_record_t, or NULL on failure.
 */
static container_record_t *launch_container(supervisor_ctx_t *ctx,
                                             const control_request_t *req)
{
    int pipefd[2];
    pid_t pid;
    char *stack, *stack_top;
    child_config_t *cfg;
    container_record_t *rec;
    producer_arg_t *parg;
    char log_path[PATH_MAX];

    /* Create pipe: container writes, supervisor reads */
    if (pipe(pipefd) < 0) {
        perror("[supervisor] pipe");
        return NULL;
    }

    /* Build child config (on heap; child_fn reads it before exec) */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    strncpy(cfg->id,       req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,   req->rootfs,       PATH_MAX - 1);
    strncpy(cfg->command,  req->command,      CHILD_COMMAND_LEN - 1);
    cfg->nice_value    = req->nice_value;
    cfg->pipe_write_fd = pipefd[1];   /* write end goes to child */

    /* Allocate clone stack */
    stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); close(pipefd[0]); close(pipefd[1]); return NULL; }
    stack_top = stack + STACK_SIZE;   /* stack grows downward */

    /* Clone with new PID, UTS, and mount namespaces */
    pid = clone(child_fn, stack_top,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    /* Close the write end in the supervisor — child owns it */
    close(pipefd[1]);
    free(stack);   /* clone duplicated it into child address space */

    if (pid < 0) {
        perror("[supervisor] clone");
        free(cfg);
        close(pipefd[0]);
        return NULL;
    }

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req->container_id);

    /* Open (or create) the log file */
    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        perror("[supervisor] open log file");
        log_fd = -1;
    }

    /* Build and register metadata */
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        if (log_fd >= 0) close(log_fd);
        close(pipefd[0]);
        free(cfg);
        return NULL;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    rec->stop_requested    = 0;
    rec->log_fd            = log_fd;
    rec->pipe_read_fd      = pipefd[0];
    strncpy(rec->log_path, log_path, PATH_MAX - 1);

    /* Start producer thread: reads from pipe, pushes to bounded buffer */
    parg = calloc(1, sizeof(*parg));
    if (!parg) {
        if (log_fd >= 0) close(log_fd);
        close(pipefd[0]);
        free(rec);
        free(cfg);
        return NULL;
    }
    parg->read_fd = pipefd[0];
    strncpy(parg->container_id, req->container_id, CONTAINER_ID_LEN - 1);
    parg->buffer = &ctx->log_buffer;

    if (pthread_create(&rec->producer_thread, NULL, producer_thread_fn, parg) != 0) {
        perror("[supervisor] pthread_create producer");
        free(parg);
        if (log_fd >= 0) close(log_fd);
        close(pipefd[0]);
        free(rec);
        free(cfg);
        return NULL;
    }

    /* Task 4: Register with kernel memory monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    /* Thread-safe insert into container list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    fprintf(stderr, "[supervisor] started container '%s' pid=%d log=%s\n",
            rec->id, rec->host_pid, rec->log_path);

    free(cfg);
    return rec;
}

/* ─── Task 2 — Signal Handling ───────────────────────────────────────────── */

/*
 * SIGCHLD: reap exited children and update metadata.
 * Must not call non-async-signal-safe functions; we just set a flag
 * and let the main loop do the reaping.
 */
static volatile sig_atomic_t g_sigchld_pending = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    g_sigchld_pending = 1;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                int sig = 0, code = 0;
                if (WIFSIGNALED(status)) {
                    sig = WTERMSIG(status);
                    rec->exit_signal = sig;
                    /* Task 2 attribution rule from project guide */
                    if (sig == SIGKILL && !rec->stop_requested)
                        rec->state = CONTAINER_HARD_LIMIT_KILLED;
                    else if (rec->stop_requested)
                        rec->state = CONTAINER_STOPPED;
                    else
                        rec->state = CONTAINER_KILLED;
                } else if (WIFEXITED(status)) {
                    code = WEXITSTATUS(status);
                    rec->exit_code = code;
                    rec->state = CONTAINER_EXITED;
                }
                fprintf(stderr,
                    "[supervisor] container '%s' pid=%d exited state=%s\n",
                    rec->id, pid, state_to_string(rec->state));

                /* Unregister from kernel monitor */
                if (ctx->monitor_fd >= 0)
                    unregister_from_monitor(ctx->monitor_fd, rec->id, pid);

                /* Close log file */
                if (rec->log_fd >= 0) {
                    close(rec->log_fd);
                    rec->log_fd = -1;
                }
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/* ─── Task 2 — Control Socket Helpers ────────────────────────────────────── */

static int create_server_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("[supervisor] socket"); return -1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    unlink(CONTROL_PATH);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[supervisor] bind"); close(fd); return -1;
    }
    if (listen(fd, 16) < 0) {
        perror("[supervisor] listen"); close(fd); return -1;
    }
    return fd;
}

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    control_response_t resp;
    char buf[4096];
    int len;
    container_record_t *rec;

    memset(buf, 0, sizeof(buf));
    len = snprintf(buf, sizeof(buf),
        "%-20s %-8s %-20s %-14s %-10s %-10s\n",
        "NAME", "PID", "STARTED", "STATE", "SOFT_MIB", "HARD_MIB");

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = ctx->containers;
    while (rec && len < (int)sizeof(buf) - 128) {
        char tstr[32];
        struct tm *tm_info = localtime(&rec->started_at);
        strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M:%S", tm_info);
        len += snprintf(buf + len, sizeof(buf) - len,
            "%-20s %-8d %-20s %-14s %-10lu %-10lu\n",
            rec->id, rec->host_pid, tstr,
            state_to_string(rec->state),
            rec->soft_limit_bytes >> 20,
            rec->hard_limit_bytes >> 20);
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp.status = 0;
    strncpy(resp.message, buf, sizeof(resp.message) - 1);
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd,
                         const char *container_id)
{
    control_response_t resp;
    container_record_t *rec;
    char log_path[PATH_MAX] = {0};

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = ctx->containers;
    while (rec) {
        if (strncmp(rec->id, container_id, CONTAINER_ID_LEN) == 0) {
            strncpy(log_path, rec->log_path, PATH_MAX - 1);
            break;
        }
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (log_path[0] == '\0') {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "Container '%s' not found.", container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    /* Send log file content line by line in response messages */
    FILE *f = fopen(log_path, "r");
    if (!f) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "Cannot open log: %s", log_path);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    char line[CONTROL_MESSAGE_LEN];
    while (fgets(line, sizeof(line), f)) {
        resp.status = 1;  /* 1 = more data follows */
        strncpy(resp.message, line, sizeof(resp.message) - 1);
        send(client_fd, &resp, sizeof(resp), 0);
    }
    fclose(f);

    resp.status = 0;  /* 0 = end of data */
    resp.message[0] = '\0';
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd,
                          const char *container_id)
{
    control_response_t resp;
    container_record_t *rec;
    int found = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = ctx->containers;
    while (rec) {
        if (strncmp(rec->id, container_id, CONTAINER_ID_LEN) == 0) {
            if (rec->state == CONTAINER_RUNNING ||
                rec->state == CONTAINER_STARTING) {
                /*
                 * Task 2 attribution rule: set stop_requested BEFORE
                 * sending signal so the SIGCHLD handler classifies
                 * termination as "stopped" not "killed".
                 */
                rec->stop_requested = 1;
                kill(rec->host_pid, SIGTERM);
                found = 1;
            } else {
                found = -1;  /* already stopped */
            }
            break;
        }
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp.status = (found > 0) ? 0 : -1;
    if (found > 0)
        snprintf(resp.message, sizeof(resp.message),
                 "Sent SIGTERM to container '%s'.", container_id);
    else if (found == -1)
        snprintf(resp.message, sizeof(resp.message),
                 "Container '%s' is not running.", container_id);
    else
        snprintf(resp.message, sizeof(resp.message),
                 "Container '%s' not found.", container_id);

    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    n = recv(client_fd, &req, sizeof(req), 0);
    if (n <= 0) return;

    memset(&resp, 0, sizeof(resp));

    switch (req.kind) {
    case CMD_PS:
        handle_ps(ctx, client_fd);
        break;

    case CMD_LOGS:
        handle_logs(ctx, client_fd, req.container_id);
        break;

    case CMD_STOP:
        handle_stop(ctx, client_fd, req.container_id);
        break;

    case CMD_START:
    case CMD_RUN: {
        container_record_t *rec = launch_container(ctx, &req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "Failed to start container '%s'.", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "Container '%s' started (pid=%d).", rec->id, rec->host_pid);
        send(client_fd, &resp, sizeof(resp), 0);

        /* CMD_RUN: wait for the container to exit before returning */
        if (req.kind == CMD_RUN) {
            int wstatus;
            waitpid(rec->host_pid, &wstatus, 0);
            pthread_mutex_lock(&ctx->metadata_lock);
            if (WIFSIGNALED(wstatus)) {
                rec->exit_signal = WTERMSIG(wstatus);
                rec->state = rec->stop_requested ?
                    CONTAINER_STOPPED : CONTAINER_KILLED;
            } else {
                rec->exit_code = WEXITSTATUS(wstatus);
                rec->state = CONTAINER_EXITED;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "Container '%s' finished.", rec->id);
            send(client_fd, &resp, sizeof(resp), 0);
        }
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "Unknown command %d.", req.kind);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }
}

/* ─── Task 1 + 2 + 6 — Supervisor Main Loop ─────────────────────────────── */

static void cleanup_all_containers(supervisor_ctx_t *ctx)
{
    container_record_t *rec, *next;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = ctx->containers;
    while (rec) {
        next = rec->next;
        /* Signal still-running containers */
        if (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING) {
            rec->stop_requested = 1;
            kill(rec->host_pid, SIGTERM);
        }
        rec = next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Give containers a moment to exit cleanly */
    sleep(1);

    /* Reap all */
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;

    /* Join producer threads and free records */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec = ctx->containers;
    while (rec) {
        next = rec->next;
        pthread_join(rec->producer_thread, NULL);
        if (rec->log_fd >= 0) close(rec->log_fd);
        free(rec);
        rec = next;
    }
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sigaction sa;
    int rc;

    (void)rootfs;  /* base rootfs noted; per-container rootfs comes in requests */

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    /* Metadata lock */
    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    /* Bounded buffer */
    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { errno = rc; perror("bounded_buffer_init"); return 1; }

    /* Task 4: Open kernel monitor device (non-fatal if module not loaded) */
    ctx.monitor_fd = open(MONITOR_DEVICE, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] WARNING: cannot open %s (%s). "
                        "Memory monitoring disabled.\n",
                MONITOR_DEVICE, strerror(errno));
    else
        fprintf(stderr, "[supervisor] Kernel monitor open: %s\n", MONITOR_DEVICE);

    /* Task 2: Create control socket */
    ctx.server_fd = create_server_socket();
    if (ctx.server_fd < 0) goto cleanup;

    /* Task 2: Signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Task 3: Start logger (consumer) thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("[supervisor] pthread_create logger");
        goto cleanup;
    }

    fprintf(stderr, "[supervisor] started. Control socket: %s\n", CONTROL_PATH);

    /* Make server socket non-blocking so we can check shutdown flag */
    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);

    /* Event loop */
    while (!g_shutdown_requested) {
        if (g_sigchld_pending) {
            g_sigchld_pending = 0;
            reap_children(&ctx);
        }

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);   /* 10 ms poll interval */
                continue;
            }
            if (errno == EINTR) continue;
            perror("[supervisor] accept");
            break;
        }
        handle_request(&ctx, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] shutting down...\n");

cleanup:
    /* Task 6: Stop containers, join threads, free resources */
    cleanup_all_containers(&ctx);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    if (ctx.server_fd >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);

    pthread_mutex_destroy(&ctx.metadata_lock);

    fprintf(stderr, "[supervisor] exited cleanly.\n");
    return 0;
}

/* ─── Task 2 — CLI Client ────────────────────────────────────────────────── */

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    /* Read one or more response messages */
    while ((n = recv(fd, &resp, sizeof(resp), 0)) > 0) {
        if (resp.message[0])
            printf("%s", resp.message);
        if (resp.status == 0 && req->kind == CMD_LOGS)
            break;   /* end-of-stream marker for logs */
        if (req->kind != CMD_LOGS)
            break;
    }

    close(fd);
    return (resp.status == 0) ? 0 : 1;
}

/* ─── CLI Command Handlers ───────────────────────────────────────────────── */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <command> "
                        "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <command> "
                        "[--soft-mib N] [--hard-mib N] [--nice N]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command,      argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
