#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <pthread.h>
#include <pthread_np.h>
#include <ps5/kernel.h>
#include <sys/stat.h>
#include <sys/cpuset.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define BYTES_PER_MB (1024ULL * 1024ULL)
#define SHELLCORE_FAN_DUTY_TO_PERCENT (100.0 / 1024.0)
#define HWINFO_AUTHID 0x4800000000000010ULL
#define MAX_CPU_CORES 8
#define MAX_CPU_LANES 16
#define MAX_TEXT 1024
#define MAX_THREAD_SAMPLES 3072
#define SOC_CLOCK_COUNT 26
#define SOC_CLOCK_GFXCLK 20
#define SOC_CLOCK_UCLK 23
#define SOC_CLOCK_FCLK 24
#define GBASE_IOCTL_GET_PIPE0_GEN 0x4004B200u
#define GBASE_IOCTL_GET_PIPE0_UB 0x4004B202u
#define ICC_FAN_OPEN_FLAGS_GET 0x10000
#define ICC_FAN_OPEN_FLAGS_SET 0x10002
#define ICC_FAN_IOCTL_GET_STATE_08 0xC01C8F08u
#define ICC_FAN_IOCTL_SET_MODE 0x80018F0Au
#define ICC_FAN_STATE_SIZE 0x1C
#define REGMGR_FAN_POLICY_MODE_KEY 0x78408C00u
#define HWINFO_CFG_WATCH false
#define HWINFO_CFG_DUMP_FAN_MODE true
#define HWINFO_CFG_SET_FAN_MODE false
#define HWINFO_CFG_FAN_MODE_RAW 0u
#define HWINFO_CFG_INTERVAL_SEC 1u
#ifndef HWINFO_BUILD_SYSINFO
#define HWINFO_BUILD_SYSINFO 1
#endif
#ifndef HWINFO_BUILD_BENCHMARK
#define HWINFO_BUILD_BENCHMARK 1
#endif
#if !HWINFO_BUILD_SYSINFO && !HWINFO_BUILD_BENCHMARK
#error At least one of HWINFO_BUILD_SYSINFO/HWINFO_BUILD_BENCHMARK must be enabled.
#endif
#define PAGE_SIZE_KB 16ULL
#define KERN_PROC_MIB_LEN 3
#define KERN_PROC_MIB_0 1
#define KERN_PROC_MIB_1 14
#define KERN_PROC_MIB_2 8
#define KINFO_PROC_ENTRY_SIZE 0x448
#define KINFO_PROC_PID_OFFSET 72
#define KINFO_PROC_VALID_OFFSET 76
#define APP_INFO_SIZE 88
#define APP_INFO_KIND_OFFSET 80
#define APP_INFO_MIN_KIND 0x1000000u
#define RESIDENT_FMEM_COUNT_SYSCALL 733
#define VM_FMEM_DOMAIN_VM0 0
#define VM_FMEM_DOMAIN_VM10 10
#define DISK_BENCH_ROOT "/data/sandbox/"
#define DISK_BENCH_ROOT_SUFFIX "-app0"
#define DISK_BENCH_DATA_ROOT "/data"
#define DISK_BENCH_SHADOW_ROOT "/mnt/shadowmnt"
#define DISK_BENCH_EXT0_ROOT "/mnt/ext0"
#define DISK_BENCH_EXT1_ROOT "/mnt/ext1"
#define DISK_BENCH_USB0_ROOT "/mnt/usb0"
#define DISK_BENCH_USB1_ROOT "/mnt/usb1"
#define DISK_BENCH_READ_CHUNK_BYTES (1024U * 1024U)
#define DISK_BENCH_FLUSH_BYTES (4U * 1024U * 1024U)
#define DISK_BENCH_PROGRESS_INTERVAL_SEC 10.0
#define DISK_BENCH_POLL_NSEC 250000000L
#define DISK_BENCH_MAX_WORKERS 8
#define DISK_BENCH_MAX_DURATION_SEC 60.0
#define DISK_BENCH_PROGRESS_BAR_WIDTH 24
#define DISK_BENCH_LOADS_PER_LINE 8
#define DISK_BENCH_FREQS_PER_LINE 4

int sceKernelGetHwModelName(char *out);
int sceKernelGetHwSerialNumber(char *out);
uint64_t sceKernelGetMainSocId(void);
long sceKernelGetCpuFrequency(void);
int sceKernelGetCpuCoreClock(int *out_mhz_per_core);
int sceKernelGetSocClock(uint32_t *out_raw_domains);
int sceKernelGetSocPowerConsumption(void *out_raw);
int sceKernelGetCpuTemperature(int *out_celsius);
int sceKernelGetSocSensorTemperature(int sensor_id, int *out_celsius);
int sceKernelGetCpuUsage(void *out, int32_t *size);
int sceKernelGetThreadName(uint32_t id, char *out);
int sceKernelGetCurrentFanDuty(uint16_t *out_duty, uint64_t *out_chassis_info);
int sceKernelIccGetUSBPowerState(uint8_t *out_state);
int sceKernelIccGetBDPowerState(uint8_t *out_state);
int sceKernelGetUniversalMode(int *out_mode);
int sceKernelGetProsperoSystemSwVersion(void *out);
int sceKernelGetAppInfo(int pid, void *out);
int sceRegMgrGetInt(uint32_t key, int *out);
int sceRegMgrGetIntInitVal(uint32_t key, int *out);
int get_page_table_stats(int vm, int type, int *total, int *free);

typedef struct orbis_timeval {
  int64_t tv_sec;
  int64_t tv_usec;
} orbis_timeval_t;

typedef struct proc_stats {
  uint32_t process_id;
  uint32_t td_tid;
  orbis_timeval_t user_cpu_usage_time;
  orbis_timeval_t system_cpu_usage_time;
} proc_stats_t;

typedef struct thread_sample {
  struct timespec wall_time;
  int32_t thread_count;
  proc_stats_t threads[MAX_THREAD_SAMPLES];
} thread_sample_t;

typedef struct memory_stats {
  int total_mb;
  int free_mb;
  int used_mb;
  double used_percent;
  bool approximate;
} memory_stats_t;

typedef struct authid_state {
  uint64_t original;
  uint64_t current;
  bool changed;
  bool available;
} authid_state_t;

typedef struct system_sw_version {
  uint32_t reserved0;
  uint32_t reserved1;
  char version[0x1c];
  uint32_t raw;
} system_sw_version_t;

typedef union soc_power_sample {
  uint8_t bytes[0x70];
  uint32_t u32[0x70 / sizeof(uint32_t)];
  uint64_t u64[0x70 / sizeof(uint64_t)];
} soc_power_sample_t;

typedef struct shellcore_state {
  int use_idle_hlt;
  int gc_gen;
  int gc_ub;
  int gfxclk_mhz;
  int fclk_mhz;
  int uclk_mhz;
  int universal_mode;
  bool have_use_idle_hlt;
  bool have_gc;
  bool have_gfxclk;
  bool have_fclk;
  bool have_uclk;
  bool have_universal_mode;
} shellcore_state_t;

typedef struct shellcore_vm_stats {
  uint64_t rss_vm0_kb;
  uint64_t rss_vm10_kb;
  uint64_t kernel_vm0_kb;
  uint64_t kernel_vm10_kb;
  uint64_t wire_vm0_kb;
  uint64_t wire_vm10_kb;
  uint64_t swap_out_kb;
  uint32_t page_table_cpu_used_mb;
  uint32_t page_table_cpu_total_mb;
  uint32_t page_table_gpu_used_mb;
  uint32_t page_table_gpu_total_mb;
  bool have_rss;
  bool have_kernel;
  bool have_wire;
  bool have_swap_out;
  bool have_page_table_cpu;
  bool have_page_table_gpu;
} shellcore_vm_stats_t;

typedef struct fan_mode_debug {
  uint8_t state_08[ICC_FAN_STATE_SIZE];
  int icc_mode_raw;
  int policy_mode_raw;
  int policy_mode_normalized;
  int policy_live_error;
  int policy_init_error;
  bool have_icc_mode;
  bool have_state_08;
  bool have_policy_mode;
  bool policy_mode_from_init;
  bool policy_mode_was_clamped;
} fan_mode_debug_t;

typedef struct runtime_options {
  bool watch;
  bool dump_fan_mode;
  bool set_fan_mode;
  uint8_t fan_mode_raw;
  unsigned interval_sec;
} runtime_options_t;

typedef struct file_entry {
  char *path;
  uint64_t size_bytes;
} file_entry_t;

typedef struct file_list {
  file_entry_t *items;
  size_t count;
  size_t capacity;
  size_t root_count;
  uint64_t total_bytes;
} file_list_t;

typedef enum disk_bench_source_kind {
  DISK_BENCH_SOURCE_APP0,
  DISK_BENCH_SOURCE_DIRECTORY
} disk_bench_source_kind_t;

typedef struct disk_bench_source {
  const char *label;
  const char *path;
  disk_bench_source_kind_t kind;
} disk_bench_source_t;

typedef struct proc_rootdir_guard {
  pid_t pid;
  intptr_t saved_rootdir;
  bool active;
} proc_rootdir_guard_t;

typedef struct cpu_monitor_state {
  thread_sample_t previous;
  uint32_t idle_ids[MAX_CPU_LANES];
  bool ready;
} cpu_monitor_state_t;

typedef struct disk_bench_snapshot {
  size_t completed_files;
  uint64_t bytes_read;
  bool failed;
  bool timed_out;
  int first_error;
  char failed_path[MAX_TEXT];
} disk_bench_snapshot_t;

typedef struct disk_bench_shared {
  const file_list_t *files;
  size_t next_index;
  size_t completed_files;
  uint64_t bytes_read;
  bool failed;
  bool stop_requested;
  bool timed_out;
  int first_error;
  char failed_path[MAX_TEXT];
  pthread_mutex_t lock;
} disk_bench_shared_t;

static double monotonic_delta_seconds(const struct timespec *start,
                                      const struct timespec *end);
static int take_thread_sample(thread_sample_t *sample);
static int resolve_idle_thread_ids(const thread_sample_t *sample,
                                   uint32_t idle_ids[MAX_CPU_LANES]);
static void compute_cpu_core_loads(const thread_sample_t *prev,
                                   const thread_sample_t *cur,
                                   const uint32_t idle_ids[MAX_CPU_LANES],
                                   int cpu_count,
                                   double out_percent[MAX_CPU_LANES]);

static int
disk_bench_affinity_cpu_for_worker(int worker_index) {
  if (worker_index < 0 || worker_index >= DISK_BENCH_MAX_WORKERS) {
    return -1;
  }

  if (MAX_CPU_LANES >= MAX_CPU_CORES * 2) {
    return worker_index * 2;
  }

  return worker_index;
}

static bool
disk_bench_can_fallback_from_affinity_error(int errnum) {
  return errnum == EPERM || errnum == ENOTSUP || errnum == EOPNOTSUPP;
}

static bool
has_suffix(const char *text, const char *suffix) {
  size_t text_len = 0;
  size_t suffix_len = 0;

  if (!text || !suffix) {
    return false;
  }

  text_len = strlen(text);
  suffix_len = strlen(suffix);
  if (text_len < suffix_len) {
    return false;
  }

  return !strcmp(text + text_len - suffix_len, suffix);
}

static char *
join_path(const char *base, const char *name) {
  size_t base_len = 0;
  size_t name_len = 0;
  bool need_slash = false;
  char *path = NULL;

  if (!base || !name) {
    errno = EINVAL;
    return NULL;
  }

  base_len = strlen(base);
  name_len = strlen(name);
  need_slash = base_len > 0 && base[base_len - 1] != '/';
  path = calloc(1, base_len + (need_slash ? 1u : 0u) + name_len + 1u);
  if (!path) {
    return NULL;
  }

  memcpy(path, base, base_len);
  if (need_slash) {
    path[base_len] = '/';
    ++base_len;
  }
  memcpy(path + base_len, name, name_len);
  return path;
}

static void
free_file_list(file_list_t *list) {
  if (!list) {
    return;
  }

  for (size_t i = 0; i < list->count; ++i) {
    free(list->items[i].path);
  }

  free(list->items);
  memset(list, 0, sizeof(*list));
}

static int
append_file_entry(file_list_t *list, const char *path, uint64_t size_bytes) {
  file_entry_t *new_items = NULL;

  if (!list || !path) {
    errno = EINVAL;
    return -1;
  }

  if (list->count == list->capacity) {
    size_t new_capacity = list->capacity ? list->capacity * 2u : 256u;

    new_items = realloc(list->items, new_capacity * sizeof(*list->items));
    if (!new_items) {
      return -1;
    }

    list->items = new_items;
    list->capacity = new_capacity;
  }

  list->items[list->count].path = strdup(path);
  if (!list->items[list->count].path) {
    return -1;
  }

  list->items[list->count].size_bytes = size_bytes;
  list->total_bytes += size_bytes;
  ++list->count;
  return 0;
}

static int
compare_file_entry_by_path(const void *lhs, const void *rhs) {
  const file_entry_t *left = lhs;
  const file_entry_t *right = rhs;

  return strcmp(left->path, right->path);
}

static int
sort_file_list(file_list_t *list) {
  if (!list) {
    errno = EINVAL;
    return -1;
  }

  if (list->count > 1u) {
    qsort(list->items,
          list->count,
          sizeof(*list->items),
          compare_file_entry_by_path);
  }

  return 0;
}

static bool
is_disk_bench_source_unavailable(int errnum) {
  return errnum == ENOENT ||
         errnum == ENOTDIR ||
         errnum == EACCES ||
         errnum == EPERM;
}

static int
collect_regular_files_recursive(file_list_t *list, const char *base_path) {
  DIR *dir = NULL;
  struct dirent *dp = NULL;
  char *path = NULL;
  int rc = -1;

  dir = opendir(base_path);
  if (!dir) {
    return -1;
  }

  while ((dp = readdir(dir)) != NULL) {
    struct stat statbuf;

    if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }

    free(path);
    path = join_path(base_path, dp->d_name);
    if (!path) {
      goto cleanup;
    }

    if (lstat(path, &statbuf)) {
      if (is_disk_bench_source_unavailable(errno)) {
        continue;
      }
      goto cleanup;
    }

    if (S_ISDIR(statbuf.st_mode)) {
      if (collect_regular_files_recursive(list, path)) {
        if (is_disk_bench_source_unavailable(errno)) {
          continue;
        }
        goto cleanup;
      }
      continue;
    }

    if (S_ISREG(statbuf.st_mode) &&
        append_file_entry(list, path, (uint64_t)statbuf.st_size)) {
      goto cleanup;
    }
  }

  rc = 0;

cleanup:
  free(path);
  closedir(dir);
  return rc;
}

static int
collect_disk_benchmark_files(file_list_t *list) {
  DIR *dir = NULL;
  struct dirent *dp = NULL;
  char *path = NULL;
  int rc = -1;

  if (!list) {
    errno = EINVAL;
    return -1;
  }

  dir = opendir(DISK_BENCH_ROOT);
  if (!dir) {
    return -1;
  }

  while ((dp = readdir(dir)) != NULL) {
    struct stat statbuf;

    if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
      continue;
    }

    if (!has_suffix(dp->d_name, DISK_BENCH_ROOT_SUFFIX)) {
      continue;
    }

    free(path);
    path = join_path(DISK_BENCH_ROOT, dp->d_name);
    if (!path) {
      goto cleanup;
    }

    if (lstat(path, &statbuf)) {
      goto cleanup;
    }

    if (!S_ISDIR(statbuf.st_mode)) {
      continue;
    }

    ++list->root_count;
    if (collect_regular_files_recursive(list, path)) {
      goto cleanup;
    }
  }

  if (sort_file_list(list)) {
    goto cleanup;
  }

  rc = 0;

cleanup:
  free(path);
  closedir(dir);
  return rc;
}

static int
collect_disk_benchmark_directory(file_list_t *list, const char *base_path) {
  struct stat statbuf;

  if (!list || !base_path) {
    errno = EINVAL;
    return -1;
  }

  if (lstat(base_path, &statbuf)) {
    return -1;
  }

  if (!S_ISDIR(statbuf.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }

  list->root_count = 1u;
  if (collect_regular_files_recursive(list, base_path) || sort_file_list(list)) {
    return -1;
  }

  return 0;
}

static int
enter_rootdir_root(proc_rootdir_guard_t *guard) {
  intptr_t root_vnode = 0;

  if (!guard) {
    errno = EINVAL;
    return -1;
  }

  memset(guard, 0, sizeof(*guard));
  guard->pid = getpid();
  guard->saved_rootdir = kernel_get_proc_rootdir(guard->pid);
  if (!guard->saved_rootdir) {
    errno = ENOENT;
    return -1;
  }

  root_vnode = kernel_get_root_vnode();
  if (!root_vnode) {
    errno = ENOENT;
    return -1;
  }

  if (kernel_set_proc_rootdir(guard->pid, root_vnode)) {
    return -1;
  }

  guard->active = true;
  return 0;
}

static void
leave_rootdir_root(proc_rootdir_guard_t *guard) {
  if (!guard || !guard->active) {
    return;
  }

  if (kernel_set_proc_rootdir(guard->pid, guard->saved_rootdir)) {
    perror("kernel_set_proc_rootdir");
  }

  guard->active = false;
}

static void
init_cpu_loads(double out_percent[MAX_CPU_LANES]) {
  for (int cpu = 0; cpu < MAX_CPU_LANES; ++cpu) {
    out_percent[cpu] = -1.0;
  }
}

static int
init_cpu_monitor(cpu_monitor_state_t *state) {
  if (!state) {
    errno = EINVAL;
    return -1;
  }

  memset(state, 0, sizeof(*state));
  if (take_thread_sample(&state->previous) ||
      resolve_idle_thread_ids(&state->previous, state->idle_ids)) {
    return -1;
  }

  state->ready = true;
  return 0;
}

static int
sample_cpu_monitor(cpu_monitor_state_t *state, double out_percent[MAX_CPU_LANES],
                   int *out_cpu_count) {
  thread_sample_t current;
  uint32_t current_idle_ids[MAX_CPU_LANES];

  if (!state || !state->ready || !out_percent || !out_cpu_count) {
    errno = EINVAL;
    return -1;
  }

  init_cpu_loads(out_percent);
  if (take_thread_sample(&current)) {
    return -1;
  }

  memset(current_idle_ids, 0, sizeof(current_idle_ids));
  if (!resolve_idle_thread_ids(&current, current_idle_ids)) {
    for (int cpu = 0; cpu < MAX_CPU_LANES; ++cpu) {
      if (state->idle_ids[cpu] == 0) {
        state->idle_ids[cpu] = current_idle_ids[cpu];
      }
    }
  }

  compute_cpu_core_loads(&state->previous,
                         &current,
                         state->idle_ids,
                         MAX_CPU_LANES,
                         out_percent);
  state->previous = current;
  *out_cpu_count = MAX_CPU_LANES;
  return 0;
}

static void
sleep_disk_bench_poll_interval(void) {
  struct timespec req = {
    .tv_sec = 0,
    .tv_nsec = DISK_BENCH_POLL_NSEC,
  };

  while (nanosleep(&req, &req) && errno == EINTR) {
  }
}

static int
init_disk_bench_shared(disk_bench_shared_t *shared, const file_list_t *files) {
  int pthread_rc = 0;

  if (!shared || !files) {
    errno = EINVAL;
    return -1;
  }

  memset(shared, 0, sizeof(*shared));
  shared->files = files;
  pthread_rc = pthread_mutex_init(&shared->lock, NULL);
  if (pthread_rc) {
    errno = pthread_rc;
    return -1;
  }

  return 0;
}

static void
destroy_disk_bench_shared(disk_bench_shared_t *shared) {
  if (!shared) {
    return;
  }

  pthread_mutex_destroy(&shared->lock);
}

static void
disk_bench_set_error(disk_bench_shared_t *shared, const char *path, int errnum) {
  pthread_mutex_lock(&shared->lock);
  if (!shared->failed) {
    shared->failed = true;
    shared->first_error = errnum ? errnum : EIO;
    snprintf(shared->failed_path,
             sizeof(shared->failed_path),
             "%s",
             path ? path : "<unknown>");
  }
  shared->stop_requested = true;
  pthread_mutex_unlock(&shared->lock);
}

static int
disk_bench_take_next_file(disk_bench_shared_t *shared, size_t *out_index) {
  int rc = -1;

  pthread_mutex_lock(&shared->lock);
  if (!shared->stop_requested && shared->next_index < shared->files->count) {
    *out_index = shared->next_index++;
    rc = 0;
  }
  pthread_mutex_unlock(&shared->lock);
  return rc;
}

static void
disk_bench_add_bytes(disk_bench_shared_t *shared, uint64_t bytes_read) {
  pthread_mutex_lock(&shared->lock);
  shared->bytes_read += bytes_read;
  pthread_mutex_unlock(&shared->lock);
}

static void
disk_bench_mark_completed(disk_bench_shared_t *shared) {
  pthread_mutex_lock(&shared->lock);
  ++shared->completed_files;
  pthread_mutex_unlock(&shared->lock);
}

static bool
disk_bench_should_stop(disk_bench_shared_t *shared) {
  bool stop = false;

  pthread_mutex_lock(&shared->lock);
  stop = shared->stop_requested;
  pthread_mutex_unlock(&shared->lock);
  return stop;
}

static void
disk_bench_request_stop(disk_bench_shared_t *shared, bool timed_out) {
  pthread_mutex_lock(&shared->lock);
  shared->stop_requested = true;
  if (timed_out) {
    shared->timed_out = true;
  }
  pthread_mutex_unlock(&shared->lock);
}

static void
disk_bench_snapshot(disk_bench_shared_t *shared, disk_bench_snapshot_t *snapshot) {
  pthread_mutex_lock(&shared->lock);
  snapshot->completed_files = shared->completed_files;
  snapshot->bytes_read = shared->bytes_read;
  snapshot->failed = shared->failed;
  snapshot->timed_out = shared->timed_out;
  snapshot->first_error = shared->first_error;
  memcpy(snapshot->failed_path,
         shared->failed_path,
         sizeof(snapshot->failed_path));
  pthread_mutex_unlock(&shared->lock);
}

static int
read_disk_bench_file(disk_bench_shared_t *shared, const char *path,
                     uint8_t *buffer, size_t buffer_size) {
  int fd = -1;
  uint64_t pending_bytes = 0;
  int saved_errno = 0;
  bool stopped = false;

  fd = open(path, O_RDONLY);
  if (fd < 0) {
    disk_bench_set_error(shared, path, errno);
    return -1;
  }

  while (true) {
    ssize_t read_rc = read(fd, buffer, buffer_size);

    if (read_rc < 0) {
      saved_errno = errno;
      break;
    }

    if (read_rc == 0) {
      break;
    }

    pending_bytes += (uint64_t)read_rc;
    if (pending_bytes >= DISK_BENCH_FLUSH_BYTES) {
      disk_bench_add_bytes(shared, pending_bytes);
      pending_bytes = 0;
    }

    if (disk_bench_should_stop(shared)) {
      stopped = true;
      break;
    }
  }

  if (pending_bytes > 0) {
    disk_bench_add_bytes(shared, pending_bytes);
  }

  if (close(fd) && !saved_errno) {
    saved_errno = errno;
  }

  if (stopped) {
    return 1;
  }

  if (saved_errno) {
    disk_bench_set_error(shared, path, saved_errno);
    errno = saved_errno;
    return -1;
  }

  return 0;
}

static void *
disk_bench_worker_main(void *arg) {
  disk_bench_shared_t *shared = arg;
  uint8_t *buffer = NULL;

  buffer = malloc(DISK_BENCH_READ_CHUNK_BYTES);
  if (!buffer) {
    disk_bench_set_error(shared, "<buffer>", ENOMEM);
    return NULL;
  }

  while (true) {
    size_t index = 0;

    if (disk_bench_take_next_file(shared, &index)) {
      break;
    }

    if (read_disk_bench_file(shared,
                             shared->files->items[index].path,
                             buffer,
                             DISK_BENCH_READ_CHUNK_BYTES)) {
      break;
    }

    disk_bench_mark_completed(shared);
  }

  free(buffer);
  return NULL;
}

static void
print_disk_bench_progress_bar(double percent) {
  int filled = 0;

  if (percent < 0.0) {
    percent = 0.0;
  } else if (percent > 100.0) {
    percent = 100.0;
  }

  filled = (int)((percent * (double)DISK_BENCH_PROGRESS_BAR_WIDTH) / 100.0);
  if (filled > DISK_BENCH_PROGRESS_BAR_WIDTH) {
    filled = DISK_BENCH_PROGRESS_BAR_WIDTH;
  }

  putchar('[');
  for (int i = 0; i < DISK_BENCH_PROGRESS_BAR_WIDTH; ++i) {
    putchar(i < filled ? '#' : '.');
  }
  putchar(']');
}

static void
format_elapsed_compact(double elapsed_sec, char out[16]) {
  unsigned total_seconds = 0;
  unsigned hours = 0;
  unsigned minutes = 0;
  unsigned seconds = 0;

  if (!out) {
    return;
  }

  if (elapsed_sec < 0.0) {
    elapsed_sec = 0.0;
  }

  total_seconds = (unsigned)(elapsed_sec + 0.5);
  hours = total_seconds / 3600u;
  minutes = (total_seconds % 3600u) / 60u;
  seconds = total_seconds % 60u;

  if (hours > 0u) {
    snprintf(out, 16, "%u:%02u:%02u", hours, minutes, seconds);
  } else {
    snprintf(out, 16, "%02u:%02u", minutes, seconds);
  }
}

static const char *
disk_bench_status_tag(const char *label) {
  if (!strcmp(label, "done")) {
    return "DONE";
  }

  if (!strcmp(label, "time")) {
    return "TIME";
  }

  if (!strcmp(label, "failed")) {
    return "FAIL";
  }

  return "RUN";
}

static void
print_cpu_load_progress(const double cpu_loads[MAX_CPU_LANES], int cpu_count) {
  if (cpu_count <= 0) {
    puts("  load  N/A");
    return;
  }

  for (int base = 0; base < cpu_count; base += DISK_BENCH_LOADS_PER_LINE) {
    int limit = base + DISK_BENCH_LOADS_PER_LINE;

    printf(base == 0 ? "  load " : "       ");
    if (limit > cpu_count) {
      limit = cpu_count;
    }

    for (int cpu = base; cpu < limit; ++cpu) {
      if (cpu_loads[cpu] < 0.0) {
        printf(" %02d: --", cpu);
      } else {
        int rounded = (int)(cpu_loads[cpu] + 0.5);

        if (rounded < 0) {
          rounded = 0;
        } else if (rounded > 100) {
          rounded = 100;
        }

        printf(" %02d:%3d%%", cpu, rounded);
      }
    }
    putchar('\n');
  }
}

static void
print_cpu_freq_progress(void) {
  int cpu_core_clocks[MAX_CPU_CORES] = {0};

  if (sceKernelGetCpuCoreClock(cpu_core_clocks)) {
    puts("  freq  N/A");
    return;
  }

  for (int base = 0; base < MAX_CPU_CORES; base += DISK_BENCH_FREQS_PER_LINE) {
    int limit = base + DISK_BENCH_FREQS_PER_LINE;

    printf(base == 0 ? "  freq " : "       ");
    if (limit > MAX_CPU_CORES) {
      limit = MAX_CPU_CORES;
    }

    for (int cpu = base; cpu < limit; ++cpu) {
      if (cpu_core_clocks[cpu] > 0) {
        printf(" %02d:%4dMHz", cpu, cpu_core_clocks[cpu]);
      } else {
        printf(" %02d: N/A", cpu);
      }
    }
    putchar('\n');
  }
}

static void
print_disk_bench_affinity(int worker_count, bool enabled, int errnum) {
  if (!enabled) {
    printf("  pin   off (%s)\n", strerror(errnum ? errnum : EPERM));
    return;
  }

  printf("  pin   ");
  for (int worker = 0; worker < worker_count; ++worker) {
    if (worker) {
      putchar(' ');
    }
    printf("T%d->%d", worker, disk_bench_affinity_cpu_for_worker(worker));
  }
  putchar('\n');
}

static void
print_disk_benchmark_status(const char *label, int worker_count,
                            const file_list_t *files,
                            const disk_bench_snapshot_t *snapshot,
                            double elapsed_sec,
                            cpu_monitor_state_t *cpu_monitor) {
  char elapsed_text[16];
  double percent = files->count
                       ? ((double)snapshot->completed_files * 100.0) /
                             (double)files->count
                       : 100.0;
  double read_mb = (double)snapshot->bytes_read / (double)BYTES_PER_MB;
  double total_mb = (double)files->total_bytes / (double)BYTES_PER_MB;
  double linear_speed_mb_s = elapsed_sec > 0.0 ? read_mb / elapsed_sec : 0.0;
  double cpu_loads[MAX_CPU_LANES];
  int cpu_count = 0;

  init_cpu_loads(cpu_loads);
  if (!cpu_monitor || sample_cpu_monitor(cpu_monitor, cpu_loads, &cpu_count)) {
    cpu_count = 0;
  }

  format_elapsed_compact(elapsed_sec, elapsed_text);
  printf("  [%s %dT %s] %5.1f%% ",
         disk_bench_status_tag(label),
         worker_count,
         elapsed_text,
         percent);
  print_disk_bench_progress_bar(percent);
  printf(" %llu/%lluf %.0f/%.0fMB %.1fMB/s\n",
         (unsigned long long)snapshot->completed_files,
         (unsigned long long)files->count,
         read_mb,
         total_mb,
         linear_speed_mb_s);
  print_cpu_load_progress(cpu_loads, cpu_count);
  print_cpu_freq_progress();

  if (snapshot->failed) {
    printf("  Error:\t %s: %s\n",
           snapshot->failed_path,
           strerror(snapshot->first_error ? snapshot->first_error : EIO));
  }
}

static int
run_disk_read_benchmark(const file_list_t *files, int worker_count) {
  pthread_t workers[DISK_BENCH_MAX_WORKERS];
  disk_bench_shared_t shared;
  disk_bench_snapshot_t snapshot;
  cpu_monitor_state_t cpu_monitor;
  struct timespec start_time;
  struct timespec last_report_time;
  struct timespec now;
  int created_workers = 0;
  int affinity_error = 0;
  int rc = -1;
  bool affinity_enabled = true;

  if (!files || !files->count || worker_count <= 0 ||
      worker_count > DISK_BENCH_MAX_WORKERS) {
    errno = EINVAL;
    return -1;
  }

  printf("Disk read benchmark (%d thread%s):\n",
         worker_count,
         worker_count == 1 ? "" : "s");
  printf("  Dataset:\t %llu files across %llu app roots, %.1f MB total\n",
         (unsigned long long)files->count,
         (unsigned long long)files->root_count,
         (double)files->total_bytes / (double)BYTES_PER_MB);
  printf("  Limit:\t %.0f s\n", DISK_BENCH_MAX_DURATION_SEC);

  if (init_disk_bench_shared(&shared, files)) {
    return -1;
  }

  memset(&snapshot, 0, sizeof(snapshot));
  memset(&cpu_monitor, 0, sizeof(cpu_monitor));
  if (clock_gettime(CLOCK_MONOTONIC, &start_time)) {
    goto cleanup;
  }
  last_report_time = start_time;

  for (int i = 0; i < worker_count; ++i) {
    int pthread_rc = 0;

    if (affinity_enabled) {
      cpuset_t cpuset;
      pthread_attr_t attr;
      int affinity_cpu = disk_bench_affinity_cpu_for_worker(i);

      if ((pthread_rc = pthread_attr_init(&attr)) != 0) {
        disk_bench_set_error(&shared, "<pthread_attr_init>", pthread_rc);
        break;
      }

      CPU_ZERO(&cpuset);
      CPU_SET(affinity_cpu, &cpuset);
      pthread_rc = pthread_attr_setaffinity_np(&attr, sizeof(cpuset), &cpuset);
      if (!pthread_rc) {
        pthread_rc = pthread_create(&workers[i], &attr, disk_bench_worker_main,
                                    &shared);
      }
      pthread_attr_destroy(&attr);

      if (pthread_rc && created_workers == 0 &&
          disk_bench_can_fallback_from_affinity_error(pthread_rc)) {
        affinity_enabled = false;
        affinity_error = pthread_rc;
        pthread_rc =
            pthread_create(&workers[i], NULL, disk_bench_worker_main, &shared);
      }
    } else {
      pthread_rc = pthread_create(&workers[i], NULL, disk_bench_worker_main,
                                  &shared);
    }

    if (pthread_rc) {
      disk_bench_set_error(&shared,
                           affinity_enabled ? "<pthread_affinity>"
                                            : "<pthread_create>",
                           pthread_rc);
      break;
    }
    ++created_workers;
  }

  print_disk_bench_affinity(worker_count, affinity_enabled, affinity_error);

  if (!init_cpu_monitor(&cpu_monitor)) {
    cpu_monitor.ready = true;
  }

  while (true) {
    disk_bench_snapshot(&shared, &snapshot);
    if (snapshot.failed || snapshot.completed_files >= files->count) {
      break;
    }

    sleep_disk_bench_poll_interval();
    if (clock_gettime(CLOCK_MONOTONIC, &now)) {
      goto cleanup;
    }

    if (monotonic_delta_seconds(&start_time, &now) >=
        DISK_BENCH_MAX_DURATION_SEC) {
      disk_bench_request_stop(&shared, true);
      break;
    }

    if (monotonic_delta_seconds(&last_report_time, &now) >=
        DISK_BENCH_PROGRESS_INTERVAL_SEC) {
      disk_bench_snapshot(&shared, &snapshot);
      print_disk_benchmark_status("progress",
                                  worker_count,
                                  files,
                                  &snapshot,
                                  monotonic_delta_seconds(&start_time, &now),
                                  cpu_monitor.ready ? &cpu_monitor : NULL);
      last_report_time = now;
    }
  }

  for (int i = 0; i < created_workers; ++i) {
    pthread_join(workers[i], NULL);
  }

  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    goto cleanup;
  }

  disk_bench_snapshot(&shared, &snapshot);
  print_disk_benchmark_status(snapshot.failed ? "failed"
                                              : (snapshot.timed_out ? "time"
                                                                    : "done"),
                              worker_count,
                              files,
                              &snapshot,
                              monotonic_delta_seconds(&start_time, &now),
                              cpu_monitor.ready ? &cpu_monitor : NULL);
  rc = snapshot.failed ? -1 : 0;

cleanup:
  destroy_disk_bench_shared(&shared);
  return rc;
}

static int
run_disk_read_benchmark_set(const char *source_label,
                            const file_list_t *files) {
  if (!source_label || !files) {
    errno = EINVAL;
    return -1;
  }

  if (!files->root_count) {
    printf("  No benchmark roots found for %s.\n", source_label);
    return 0;
  }

  if (!files->count) {
    printf("  Found %llu root%s, but no regular files to read in %s.\n",
           (unsigned long long)files->root_count,
           files->root_count == 1u ? "" : "s",
           source_label);
    return 0;
  }

  printf("  Source:\t %s\n", source_label);
  if (run_disk_read_benchmark(files, 1) ||
      run_disk_read_benchmark(files, DISK_BENCH_MAX_WORKERS)) {
    return -1;
  }

  return 0;
}

static int
collect_disk_benchmark_source(const disk_bench_source_t *source,
                              file_list_t *files) {
  if (!source || !files) {
    errno = EINVAL;
    return -1;
  }

  switch (source->kind) {
    case DISK_BENCH_SOURCE_APP0:
      return collect_disk_benchmark_files(files);
    case DISK_BENCH_SOURCE_DIRECTORY:
      return collect_disk_benchmark_directory(files, source->path);
    default:
      errno = EINVAL;
      return -1;
  }
}

static int
run_disk_read_benchmark_source(const disk_bench_source_t *source,
                               bool *out_ran_benchmark) {
  file_list_t files = {0};
  int saved_errno = 0;
  int rc = -1;

  if (!source) {
    errno = EINVAL;
    return -1;
  }

  if (out_ran_benchmark) {
    *out_ran_benchmark = false;
  }

  if (collect_disk_benchmark_source(source, &files)) {
    saved_errno = errno;
    if (is_disk_bench_source_unavailable(saved_errno)) {
      printf("  Skipping %s: %s\n", source->label, strerror(saved_errno));
      rc = 0;
      goto cleanup;
    }

    printf("  Failed to collect %s: %s\n",
           source->label,
           strerror(saved_errno));
    goto cleanup;
  }

  if (run_disk_read_benchmark_set(source->label, &files)) {
    goto cleanup;
  }

  if (files.count && out_ran_benchmark) {
    *out_ran_benchmark = true;
  }

  rc = 0;

cleanup:
  free_file_list(&files);
  if (rc && saved_errno) {
    errno = saved_errno;
  }
  return rc;
}

static int
run_disk_read_benchmarks(void) {
  static const disk_bench_source_t sources[] = {
    {
      DISK_BENCH_ROOT "/*" DISK_BENCH_ROOT_SUFFIX,
      DISK_BENCH_ROOT,
      DISK_BENCH_SOURCE_APP0
    },
    {DISK_BENCH_DATA_ROOT, DISK_BENCH_DATA_ROOT, DISK_BENCH_SOURCE_DIRECTORY},
    {DISK_BENCH_SHADOW_ROOT,
     DISK_BENCH_SHADOW_ROOT,
     DISK_BENCH_SOURCE_DIRECTORY},
    {DISK_BENCH_EXT0_ROOT, DISK_BENCH_EXT0_ROOT, DISK_BENCH_SOURCE_DIRECTORY},
    {DISK_BENCH_EXT1_ROOT, DISK_BENCH_EXT1_ROOT, DISK_BENCH_SOURCE_DIRECTORY},
    {DISK_BENCH_USB0_ROOT, DISK_BENCH_USB0_ROOT, DISK_BENCH_SOURCE_DIRECTORY},
    {DISK_BENCH_USB1_ROOT, DISK_BENCH_USB1_ROOT, DISK_BENCH_SOURCE_DIRECTORY}
  };
  proc_rootdir_guard_t guard = {0};
  bool have_rootdir_guard = false;
  bool ran_any_benchmark = false;
  int rc = -1;

  puts("Disk benchmarks:");
  if (!enter_rootdir_root(&guard)) {
    have_rootdir_guard = true;
  }

  for (size_t i = 0; i < sizeof(sources) / sizeof(sources[0]); ++i) {
    bool ran_source_benchmark = false;

    if (run_disk_read_benchmark_source(&sources[i], &ran_source_benchmark)) {
      goto cleanup;
    }

    ran_any_benchmark = ran_any_benchmark || ran_source_benchmark;
  }

  if (!ran_any_benchmark) {
    puts("  No disk benchmark sources with readable regular files were found.");
  }

  rc = 0;

cleanup:
  if (have_rootdir_guard) {
    leave_rootdir_root(&guard);
  }
  return rc;
}

static int
read_int_sysctl(const char *name, int *value) {
  size_t len = sizeof(*value);

  memset(value, 0, sizeof(*value));
  if (sysctlbyname(name, value, &len, NULL, 0)) {
    return -1;
  }

  return 0;
}

static int
read_uint64_sysctl(const char *name, uint64_t *value) {
  size_t len = sizeof(*value);

  memset(value, 0, sizeof(*value));
  if (sysctlbyname(name, value, &len, NULL, 0)) {
    return -1;
  }

  return 0;
}

static int
read_int_sysctl_with_input(const char *name, uint32_t selector, int *value) {
  size_t len = sizeof(*value);

  memset(value, 0, sizeof(*value));
  if (sysctlbyname(name, value, &len, &selector, sizeof(selector))) {
    return -1;
  }

  return 0;
}

static int
read_icc_fan_blob(unsigned long request, uint8_t out[ICC_FAN_STATE_SIZE]) {
  int fd = -1;
  int rc = -1;
  int saved_errno = 0;

  memset(out, 0, ICC_FAN_STATE_SIZE);
  fd = open("/dev/icc_fan", ICC_FAN_OPEN_FLAGS_GET);
  if (fd < 0) {
    return -1;
  }

  if (ioctl(fd, request, out) < 0) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }

  rc = 0;
  close(fd);
  return rc;
}

static int
set_icc_fan_mode_raw(uint8_t mode) {
  int fd = -1;
  int saved_errno = 0;

  fd = open("/dev/icc_fan", ICC_FAN_OPEN_FLAGS_SET);
  if (fd < 0) {
    return -1;
  }

  if (ioctl(fd, ICC_FAN_IOCTL_SET_MODE, &mode) < 0) {
    saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }

  close(fd);
  return 0;
}

static void
read_fan_policy_mode(fan_mode_debug_t *debug) {
  int raw = 0;
  int rc = sceRegMgrGetInt(REGMGR_FAN_POLICY_MODE_KEY, &raw);

  debug->policy_live_error = rc;
  if (rc < 0) {
    raw = 0;
    rc = sceRegMgrGetIntInitVal(REGMGR_FAN_POLICY_MODE_KEY, &raw);
    debug->policy_init_error = rc;
    if (rc < 0) {
      return;
    }
    debug->policy_mode_from_init = true;
  }

  debug->policy_mode_raw = raw;
  if (raw < 0 || raw >= 5) {
    debug->policy_mode_normalized = 0;
    debug->policy_mode_was_clamped = true;
  } else {
    debug->policy_mode_normalized = raw;
  }
  debug->have_policy_mode = true;
}

static void
decode_fan_state_08(fan_mode_debug_t *debug) {
  uint32_t raw_mode = 0;

  memcpy(&raw_mode, debug->state_08, sizeof(raw_mode));
  if (raw_mode <= 3u) {
    debug->icc_mode_raw = (int)raw_mode;
    debug->have_icc_mode = true;
  }
}

static void
read_fan_mode_debug(fan_mode_debug_t *debug, bool include_state_08) {
  bool need_state_08 = false;

  memset(debug, 0, sizeof(*debug));

  read_fan_policy_mode(debug);
  need_state_08 = include_state_08 || !debug->have_policy_mode;
  if (need_state_08 &&
      !read_icc_fan_blob(ICC_FAN_IOCTL_GET_STATE_08, debug->state_08)) {
    debug->have_state_08 = true;
    decode_fan_state_08(debug);
  }
}

static int
ensure_process_authid(authid_state_t *state) {
  pid_t pid = getpid();
  uint64_t current = kernel_get_ucred_authid(pid);

  memset(state, 0, sizeof(*state));
  if (!current) {
    return -1;
  }

  state->original = current;
  state->current = current;
  state->available = true;

  if (current == HWINFO_AUTHID) {
    return 0;
  }

  if (kernel_set_ucred_authid(pid, HWINFO_AUTHID)) {
    return -1;
  }

  current = kernel_get_ucred_authid(pid);
  if (!current) {
    current = HWINFO_AUTHID;
  }

  state->current = current;
  state->changed = state->current != state->original;
  return 0;
}

static int
read_gbase_pipe0_link(int *out_gen, int *out_ub) {
  int fd = -1;
  uint32_t gen = 0;
  uint32_t ub = 0;
  int rc = -1;

  fd = open("/dev/gbase", O_RDONLY);
  if (fd < 0) {
    return -1;
  }

  if (ioctl(fd, GBASE_IOCTL_GET_PIPE0_GEN, &gen)) {
    goto cleanup;
  }

  if (ioctl(fd, GBASE_IOCTL_GET_PIPE0_UB, &ub)) {
    ub = 0;
  }

  *out_gen = (int)gen;
  *out_ub = ub ? 1 : 0;
  rc = 0;

cleanup:
  close(fd);
  return rc;
}

static const char *
bapm_mode_name(int mode) {
  switch (mode) {
  case -1:
    return "off";
  case 0:
    return "host";
  case 1:
    return "vsh";
  case 2:
    return "safemode";
  case 3:
    return "standby";
  case 4:
    return "kratos";
  case 5:
    return "vsh nokeyx";
  case 6:
    return "standby nokeyx";
  default:
    return NULL;
  }
}

static void
read_shellcore_state(const uint32_t soc_clocks[26], shellcore_state_t *state) {
  memset(state, 0, sizeof(*state));

  if (!read_int_sysctl("machdep.use_idle_hlt", &state->use_idle_hlt)) {
    state->have_use_idle_hlt = true;
  }

  if (!read_gbase_pipe0_link(&state->gc_gen, &state->gc_ub)) {
    state->have_gc = true;
  }

  if (SOC_CLOCK_GFXCLK < SOC_CLOCK_COUNT && soc_clocks[SOC_CLOCK_GFXCLK] > 0) {
    state->gfxclk_mhz = (int)soc_clocks[SOC_CLOCK_GFXCLK];
    state->have_gfxclk = true;
  }

  if (SOC_CLOCK_FCLK < SOC_CLOCK_COUNT && soc_clocks[SOC_CLOCK_FCLK] > 0) {
    state->fclk_mhz = (int)soc_clocks[SOC_CLOCK_FCLK];
    state->have_fclk = true;
  }

  if (SOC_CLOCK_UCLK < SOC_CLOCK_COUNT && soc_clocks[SOC_CLOCK_UCLK] > 0) {
    state->uclk_mhz = (int)soc_clocks[SOC_CLOCK_UCLK];
    state->have_uclk = true;
  }

  if (!sceKernelGetUniversalMode(&state->universal_mode)) {
    state->have_universal_mode = true;
  }
}

static int
get_resident_fmem_count_by_type(uint32_t pid, int type,
                                uint64_t *out_size_pages,
                                uint64_t *out_avail_pages) {
  long rc = -1;
  uint64_t size_pages = 0;
  uint64_t avail_pages = 0;

  rc = syscall(RESIDENT_FMEM_COUNT_SYSCALL,
               (long)pid,
               (long)type,
               &size_pages,
               &avail_pages);
  if (rc == -1) {
    return -1;
  }

  *out_size_pages = size_pages;
  *out_avail_pages = avail_pages;
  return 0;
}

static bool
is_shellcore_vm_app_process(uint32_t pid) {
  uint8_t app_info[APP_INFO_SIZE];
  uint32_t app_kind = 0;

  memset(app_info, 0, sizeof(app_info));
  if (sceKernelGetAppInfo((int)pid, app_info)) {
    return false;
  }

  memcpy(&app_kind, app_info + APP_INFO_KIND_OFFSET, sizeof(app_kind));
  return app_kind >= APP_INFO_MIN_KIND;
}

static int
read_shellcore_rss_kb(uint64_t *out_vm0_kb, uint64_t *out_vm10_kb) {
  int mib[KERN_PROC_MIB_LEN] = { KERN_PROC_MIB_0, KERN_PROC_MIB_1,
                                 KERN_PROC_MIB_2 };
  uint8_t *proc_buf = NULL;
  size_t proc_buf_len = 0;
  uint64_t vm0_pages = 0;
  uint64_t vm10_pages = 0;
  int rc = -1;

  if (sysctl(mib, KERN_PROC_MIB_LEN, NULL, &proc_buf_len, NULL, 0)) {
    goto cleanup;
  }

  if (proc_buf_len < KINFO_PROC_ENTRY_SIZE) {
    errno = EIO;
    goto cleanup;
  }

  proc_buf = calloc(1, proc_buf_len);
  if (!proc_buf) {
    goto cleanup;
  }

  if (sysctl(mib, KERN_PROC_MIB_LEN, proc_buf, &proc_buf_len, NULL, 0)) {
    goto cleanup;
  }

  for (size_t offset = 0; offset + KINFO_PROC_ENTRY_SIZE <= proc_buf_len;
       offset += KINFO_PROC_ENTRY_SIZE) {
    uint32_t pid = 0;
    uint32_t marker = 0;
    uint64_t vm0_size_pages = 0;
    uint64_t vm10_size_pages = 0;
    uint64_t ignored_pages = 0;

    memcpy(&pid, proc_buf + offset + KINFO_PROC_PID_OFFSET, sizeof(pid));
    memcpy(&marker, proc_buf + offset + KINFO_PROC_VALID_OFFSET,
           sizeof(marker));
    if (!pid || !marker || !is_shellcore_vm_app_process(pid)) {
      continue;
    }

    if (get_resident_fmem_count_by_type(pid,
                                        VM_FMEM_DOMAIN_VM0,
                                        &vm0_size_pages,
                                        &ignored_pages) ||
        get_resident_fmem_count_by_type(pid,
                                        VM_FMEM_DOMAIN_VM10,
                                        &vm10_size_pages,
                                        &ignored_pages)) {
      continue;
    }

    vm0_pages += vm0_size_pages;
    vm10_pages += vm10_size_pages;
  }

  *out_vm0_kb = vm0_pages * PAGE_SIZE_KB;
  *out_vm10_kb = vm10_pages * PAGE_SIZE_KB;
  rc = 0;

cleanup:
  free(proc_buf);
  return rc;
}

static int
read_kernel_page_count_kb(uint32_t selector, uint64_t *out_kb) {
  int pages = 0;

  if (read_int_sysctl_with_input("vm.kern_page_count_in_cnt", selector,
                                 &pages) ||
      pages < 0) {
    return -1;
  }

  *out_kb = (uint64_t)(unsigned int)pages * PAGE_SIZE_KB;
  return 0;
}

static int
read_wire_count_kb(const char *name, uint64_t *out_kb) {
  int pages = 0;

  if (read_int_sysctl(name, &pages) || pages < 0) {
    return -1;
  }

  *out_kb = (uint64_t)(unsigned int)pages * PAGE_SIZE_KB;
  return 0;
}

static int
read_swap_out_kb(uint64_t *out_kb) {
  uint64_t swap_total_bytes = 0;
  int swap_avail_pages = 0;
  uint64_t swap_avail_kb = 0;
  uint64_t swap_total_kb = 0;

  if (read_uint64_sysctl("vm.swap_total", &swap_total_bytes) ||
      read_int_sysctl("vm.swap_avail", &swap_avail_pages) ||
      swap_avail_pages < 0) {
    return -1;
  }

  swap_total_kb = swap_total_bytes >> 10;
  swap_avail_kb = (uint64_t)(unsigned int)swap_avail_pages * PAGE_SIZE_KB;
  *out_kb = swap_total_kb > swap_avail_kb ? swap_total_kb - swap_avail_kb : 0;
  return 0;
}

static void
read_shellcore_vm_stats(shellcore_vm_stats_t *stats) {
  int cpu_total_mb = 0;
  int cpu_free_mb = 0;
  int gpu_total_mb = 0;
  int gpu_free_mb = 0;

  memset(stats, 0, sizeof(*stats));

  if (!read_shellcore_rss_kb(&stats->rss_vm0_kb, &stats->rss_vm10_kb)) {
    stats->have_rss = true;
  }

  if (!read_kernel_page_count_kb(VM_FMEM_DOMAIN_VM0, &stats->kernel_vm0_kb) &&
      !read_kernel_page_count_kb(VM_FMEM_DOMAIN_VM10,
                                 &stats->kernel_vm10_kb)) {
    stats->have_kernel = true;
  }

  if (!read_wire_count_kb("vm.stats.vm0.v_wire_count", &stats->wire_vm0_kb) &&
      !read_wire_count_kb("vm.stats.vm10.v_wire_count",
                          &stats->wire_vm10_kb)) {
    stats->have_wire = true;
  }

  if (!read_swap_out_kb(&stats->swap_out_kb)) {
    stats->have_swap_out = true;
  }

  if (!get_page_table_stats(0, 1, &cpu_total_mb, &cpu_free_mb) &&
      cpu_total_mb >= 0 && cpu_free_mb >= 0 && cpu_free_mb <= cpu_total_mb) {
    stats->page_table_cpu_total_mb = (uint32_t)cpu_total_mb;
    stats->page_table_cpu_used_mb = (uint32_t)(cpu_total_mb - cpu_free_mb);
    stats->have_page_table_cpu = true;
  }

  if (!get_page_table_stats(0, 2, &gpu_total_mb, &gpu_free_mb) &&
      gpu_total_mb >= 0 && gpu_free_mb >= 0 && gpu_free_mb <= gpu_total_mb) {
    stats->page_table_gpu_total_mb = (uint32_t)gpu_total_mb;
    stats->page_table_gpu_used_mb = (uint32_t)(gpu_total_mb - gpu_free_mb);
    stats->have_page_table_gpu = true;
  }
}

static void
print_shellcore_vm_stats(const shellcore_vm_stats_t *stats) {
  puts("VM Stats:");

  if (stats->have_rss) {
    printf("  RSS vm0/vm10\t %.1f / %.1f MB\n",
           (double)stats->rss_vm0_kb / 1024.0,
           (double)stats->rss_vm10_kb / 1024.0);
  } else {
    puts("  RSS vm0/vm10\t N/A");
  }

  if (stats->have_kernel) {
    printf("  kernel vm0/vm10\t %.1f / %.1f MB\n",
           (double)stats->kernel_vm0_kb / 1024.0,
           (double)stats->kernel_vm10_kb / 1024.0);
  } else {
    puts("  kernel vm0/vm10\t N/A");
  }

  if (stats->have_wire) {
    printf("  wire vm0/vm10\t %.1f / %.1f MB\n",
           (double)stats->wire_vm0_kb / 1024.0,
           (double)stats->wire_vm10_kb / 1024.0);
  } else {
    puts("  wire vm0/vm10\t N/A");
  }

  if (stats->have_swap_out) {
    printf("  swap out\t %.1f MB\n", (double)stats->swap_out_kb / 1024.0);
  } else {
    puts("  swap out\t N/A");
  }

  if (stats->have_page_table_cpu) {
    printf("  page table CPU\t %u / %u MB\n",
           stats->page_table_cpu_used_mb,
           stats->page_table_cpu_total_mb);
  } else {
    puts("  page table CPU\t N/A");
  }

  if (stats->have_page_table_gpu) {
    printf("  page table GPU\t %u / %u MB\n",
           stats->page_table_gpu_used_mb,
           stats->page_table_gpu_total_mb);
  } else {
    puts("  page table GPU\t N/A");
  }

  puts("VM note:\t paired values vm0/vm10 split.");
}

static void
set_memory_stats_from_bytes(memory_stats_t *stats, uint64_t total_bytes,
                            uint64_t free_bytes, bool approximate) {
  uint64_t total_mb = total_bytes / BYTES_PER_MB;
  uint64_t free_mb = free_bytes / BYTES_PER_MB;

  if (free_mb > total_mb) {
    free_mb = total_mb;
  }

  stats->total_mb = (int)total_mb;
  stats->free_mb = (int)free_mb;
  stats->used_mb = stats->total_mb - stats->free_mb;
  stats->used_percent = stats->total_mb > 0
                            ? ((double)stats->used_mb * 100.0) /
                                  (double)stats->total_mb
                            : 0.0;
  stats->approximate = approximate;
}

static int
read_page_table_memory_stats(int vm, int type, memory_stats_t *stats) {
  int total_mb = 0;
  int free_mb = 0;

  if (get_page_table_stats(vm, type, &total_mb, &free_mb)) {
    return -1;
  }

  if (total_mb <= 0) {
    return -1;
  }

  if (free_mb < 0) {
    free_mb = 0;
  }

  if (free_mb > total_mb) {
    free_mb = total_mb;
  }

  stats->total_mb = total_mb;
  stats->free_mb = free_mb;
  stats->used_mb = total_mb - free_mb;
  stats->used_percent = ((double)stats->used_mb * 100.0) / (double)total_mb;
  stats->approximate = false;

  return 0;
}

static int
read_system_ram_sysctl(memory_stats_t *stats) {
  uint64_t physmem = 0;
  uint64_t pagesize = 0;
  uint64_t page_count = 0;
  uint64_t free_count = 0;
  uint64_t inactive_count = 0;
  uint64_t cache_count = 0;
  uint64_t total_bytes = 0;
  uint64_t reclaimable_pages = 0;

  if (read_uint64_sysctl("hw.pagesize", &pagesize) || pagesize == 0) {
    return -1;
  }

  if (!read_uint64_sysctl("vm.stats.vm.v_page_count", &page_count) &&
      page_count > 0) {
    total_bytes = page_count * pagesize;
  } else if (!read_uint64_sysctl("hw.physmem", &physmem) && physmem > 0) { // [SYSCTL] Error : hw.physmem is not approved.
    total_bytes = physmem;
  } else if (!read_uint64_sysctl("hw.realmem", &physmem) && physmem > 0) { // [SYSCTL] Error : hw.realmem is not approved.
    total_bytes = physmem;
  } else if (!read_uint64_sysctl("hw.usermem", &physmem) && physmem > 0) {
    total_bytes = physmem;
  } else {
    return -1;
  }

  if (!read_uint64_sysctl("vm.stats.vm.v_free_count", &free_count)) {
    reclaimable_pages += free_count;
  }
  if (!read_uint64_sysctl("vm.stats.vm.v_inactive_count", &inactive_count)) {
    reclaimable_pages += inactive_count;
  }
  if (!read_uint64_sysctl("vm.stats.vm.v_cache_count", &cache_count)) {
    reclaimable_pages += cache_count;
  }

  if (reclaimable_pages == 0) {
    return -1;
  }

  set_memory_stats_from_bytes(stats, total_bytes, reclaimable_pages * pagesize,
                              true);
  return 0;
}

static int
read_system_ram_stats(memory_stats_t *stats) {
  if (!read_page_table_memory_stats(1, 1, stats)) {
    return 0;
  }

  return read_system_ram_sysctl(stats);
}

static void
print_memory_stats(const char *label, const memory_stats_t *stats,
                   const char *suffix) {
  printf("%s:\t %d / %d %s used (%.1f%%), free %d %s\n",
         label,
         stats->used_mb,
         stats->total_mb,
         suffix,
         stats->used_percent,
         stats->free_mb,
         suffix);
}

static void
print_fan_duty(uint16_t raw_duty) {
  double percent = (double)raw_duty * SHELLCORE_FAN_DUTY_TO_PERCENT;

  printf("Fan duty:\t %.2f%%\n", percent);
  printf("Fan duty raw:\t %u\n", (unsigned)raw_duty);
  puts("Fan note:\t Formula: raw * 100 / 1024.");
}

static void
print_hex_blob_line(const char *label, const uint8_t *blob, size_t size) {
  printf("  %-15s", label);
  for (size_t i = 0; i < size; ++i) {
    printf("%s%02x", i ? " " : "", blob[i]);
  }
  putchar('\n');
}

static void
print_u32_blob_line(const char *label, const uint8_t *blob, size_t size) {
  printf("  %-15s", label);
  for (size_t i = 0; i + sizeof(uint32_t) <= size; i += sizeof(uint32_t)) {
    uint32_t value = 0;
    memcpy(&value, blob + i, sizeof(value));
    printf("%s0x%08x", i ? " " : "", value);
  }
  putchar('\n');
}

static const char *
fan_policy_mode_name(int normalized_mode) {
  switch (normalized_mode) {
  case 0:
    return "auto";
  case 1:
    return "compat";
  case 2:
    return "fixed";
  case 3:
  case 4:
    return "fallback";
  default:
    return NULL;
  }
}

static void
print_fan_policy_source(const fan_mode_debug_t *debug) {
  if (debug->have_policy_mode && debug->policy_mode_from_init) {
    puts("Fan mode src:\t policy reg 0x78408C00 (init/default fallback).");
  } else if (debug->have_policy_mode) {
    puts("Fan mode src:\t policy reg 0x78408C00 (live).");
  } else if (debug->have_icc_mode) {
    printf("Fan mode src:\t get 8F08 dword0 = %d (ICC).\n",
           debug->icc_mode_raw);
    if (debug->policy_live_error < 0 || debug->policy_init_error < 0) {
      printf("\t\t policy reg live err 0x%08x, init err 0x%08x.\n",
             (unsigned)debug->policy_live_error,
             (unsigned)debug->policy_init_error);
    }
  } else {
    puts("Fan mode src:\t unavailable.");
  }
 /* puts("\t\t ICC servo raw 0..3");
  puts("\t\t direct 0xC01C8F08 decoding is still inferred,");
  puts("\t\t not confirmed by a dedicated getter symbol."); */
}

static void
print_fan_mode_summary(const fan_mode_debug_t *debug, bool set_fan_mode,
                       uint8_t fan_mode_raw) {
  const char *policy_name = NULL;

  if (debug->have_policy_mode) {
    policy_name = fan_policy_mode_name(debug->policy_mode_normalized);
    if (policy_name) {
      printf("Fan mode:\t %s (%s policy)\n",
             policy_name,
             debug->policy_mode_from_init ? "init" : "live");
    } else {
      printf("Fan mode:\t raw %d (%s policy)\n",
             debug->policy_mode_normalized,
             debug->policy_mode_from_init ? "init" : "live");
    }

    if (debug->policy_mode_from_init && debug->policy_live_error < 0) {
      if (debug->policy_mode_was_clamped) {
        printf("Fan mode raw:\t reg 0x%08x live err 0x%08x, init %d -> %d\n",
               REGMGR_FAN_POLICY_MODE_KEY,
               (unsigned)debug->policy_live_error,
               debug->policy_mode_raw,
               debug->policy_mode_normalized);
      } else {
        printf("Fan mode raw:\t reg 0x%08x live err 0x%08x, init %d\n",
               REGMGR_FAN_POLICY_MODE_KEY,
               (unsigned)debug->policy_live_error,
               debug->policy_mode_raw);
      }
    } else if (debug->policy_mode_was_clamped) {
      printf("Fan mode raw:\t reg 0x%08x = %d, normalized to %d\n",
             REGMGR_FAN_POLICY_MODE_KEY,
             debug->policy_mode_raw,
             debug->policy_mode_normalized);
    } else {
      printf("Fan mode raw:\t reg 0x%08x = %d\n",
             REGMGR_FAN_POLICY_MODE_KEY,
             debug->policy_mode_raw);
    }
  } else if (set_fan_mode) {
    printf("Fan mode:\t raw %u (requested)\n", (unsigned)fan_mode_raw);
    if (debug->policy_live_error < 0 || debug->policy_init_error < 0) {
      printf("Fan mode raw:\t policy reg live err 0x%08x, init err 0x%08x\n",
             (unsigned)debug->policy_live_error,
             (unsigned)debug->policy_init_error);
    } else {
      puts("Fan mode raw:\t policy reg N/A");
    }
  } else if (debug->have_icc_mode) {
    printf("Fan mode:\t raw %d (icc pattern, inferred)\n",
           debug->icc_mode_raw);
    printf("Fan mode raw:\t get 8F08 dword0 = %d\n", debug->icc_mode_raw);
  } else {
    puts("Fan mode:\t unknown");
    if (debug->policy_live_error < 0 || debug->policy_init_error < 0) {
      printf("Fan mode raw:\t reg 0x%08x live err 0x%08x, init err 0x%08x\n",
             REGMGR_FAN_POLICY_MODE_KEY,
             (unsigned)debug->policy_live_error,
             (unsigned)debug->policy_init_error);
    } else {
      puts("Fan mode raw:\t N/A");
    }
  }

  if (set_fan_mode) {
    printf("Fan mode set:\t raw %u (0x%02x)\n",
           (unsigned)fan_mode_raw,
           (unsigned)fan_mode_raw);
  }

  print_fan_policy_source(debug);
}

static void
print_fan_mode_debug(const fan_mode_debug_t *debug, bool set_fan_mode,
                     uint8_t fan_mode_raw) {
  const char *policy_name = NULL;

  puts("Fan ICC state:");
  if (debug->have_policy_mode) {
    policy_name = fan_policy_mode_name(debug->policy_mode_normalized);
    printf("  policy raw\t %d -> %d (%s, %s)\n",
           debug->policy_mode_raw,
           debug->policy_mode_normalized,
           policy_name ? policy_name : "unknown",
           debug->policy_mode_from_init ? "init" : "live");
  } else {
    printf("  policy raw\t live err 0x%08x, init err 0x%08x\n",
           (unsigned)debug->policy_live_error,
           (unsigned)debug->policy_init_error);
  }
  if (debug->have_icc_mode) {
    printf("  icc raw\t %d (from get 8F08 dword0, inferred)\n",
           debug->icc_mode_raw);
  } else {
    puts("  icc raw\t N/A");
  }
  if (set_fan_mode) {
    printf("  set mode raw\t %u (0x%02x)\n",
           (unsigned)fan_mode_raw,
           (unsigned)fan_mode_raw);
  }

  if (debug->have_state_08) {
    print_hex_blob_line("get 8F08 bytes", debug->state_08, ICC_FAN_STATE_SIZE);
    print_u32_blob_line("get 8F08 u32", debug->state_08, ICC_FAN_STATE_SIZE);
  } else {
    puts("  get 8F08\t N/A");
  }
/*
  puts("Fan mode note:\t policy labels are inferred from ShellCore");
  puts("\t\t branch names: raw 0 auto, 1 compat, 2 fixed,");
  puts("\t\t raw 3/4 share the same fallback builder.");
  puts("\t\t live reg read falls back to GetIntInitVal().");
  puts("\t\t if reg access is blocked, get 8F08 dword0 is used");
  puts("\t\t as an inferred ICC raw pattern when it is 0..3.");
  puts("\t\t 0x80018F0A sets ICC servo pattern raw 0..3,");
  puts("\t\t 0xC01C8F08 is the 0x1c-byte state blob, and");
  puts("\t\t 0xC01C8F07 should not be probed as a getter."); */
}

static void
print_shellcore_state(const shellcore_state_t *state, uint64_t main_soc_id) {
  const char *bapm_name = NULL;

  if (state->have_use_idle_hlt) {
    printf("  HLT\t\t %d\n", state->use_idle_hlt);
  } else {
    puts("  HLT\t\t N/A");
  }

  if (state->have_gc) {
    printf("  GC\t\t gen%d%s\n", state->gc_gen, state->gc_ub ? "UB" : "");
  } else {
    puts("  GC\t\t N/A");
  }

  if (state->have_gfxclk) {
    printf("  GFXCLK\t %d MHz\n", state->gfxclk_mhz);
  } else {
    puts("  GFXCLK\t N/A");
  }

  if (state->have_fclk) {
    printf("  FCLK\t\t %d MHz\n", state->fclk_mhz);
  } else {
    puts("  FCLK\t\t N/A");
  }

  if (state->have_uclk) {
    printf("  UCLK\t\t %d MHz\n", state->uclk_mhz);
  } else {
    puts("  UCLK\t\t N/A");
  }

  if (main_soc_id) {
    printf("  ID\t\t %08x\n", (unsigned)(uint32_t)main_soc_id);
  } else {
    puts("  ID\t\t N/A");
  }
  
  if (state->have_universal_mode) {
    bapm_name = bapm_mode_name(state->universal_mode);
    if (bapm_name) {
      printf("  BAPM\t\t %s (raw %d)\n", bapm_name, state->universal_mode);
    } else {
      printf("  BAPM\t\t raw %d\n", state->universal_mode);
    }
  } else {
    puts("  BAPM\t\t N/A");
  }

  puts("BAPM note:\t Formats sceKernelGetUniversalMode() as");
  puts("\t\t -1 off, 0 host, 1 vsh, 2 safemode, 3 standby,");
  puts("\t\t 4 kratos, 5 vsh nokeyx, 6 standby nokeyx.");
}

static void
print_soc_clock_entry(int index, uint32_t raw_value) {
  const char *label = NULL;

  switch (index) {
  case 0:
    label = "SOCCLK?";
    break;
  case 1:
    label = "REFCLK?";
    break;
  case 2:
    label = "DCEFCLK?";
    break;
  case 3:
    label = "Display 0?";
    break;
  case 4:
    label = "Display 1?";
    break;
  case 5:
    label = "MCLK?";
    break;
  case 6:
    label = "Fabric Aux 0?";
    break;
  case 7:
    label = "Fabric Aux 1?";
    break;
  case 8:
    label = "REFCLK 500B?";
    break;
  case 9:
    label = "Display 2?";
    break;
  case 10:
    label = "Fabric Aux 2?";
    break;
  case 11:
    label = "VCN VCLK?";
    break;
  case 12:
    label = "VCN DCLK?";
    break;
  case 13:
    label = "Disp Engine?";
    break;
  case 14:
    label = "DISPCLK?";
    break;
  case 15:
    label = "GPU L2/L3 Cache";
    break;
  case 16:
    label = "SOCCLK Aux?";
    break;
  case 17:
    label = "System Bus LP0";
    break;
  case 18:
    label = "System Bus LP1";
    break;
  case 19:
    label = "PCLK?/AUX?";
    break;
  case 20:
    label = "GFXCLK";
    break;
  case 21:
    label = "GFX Aux 0?";
    break;
  case 22:
    label = "GFX Aux 1?";
    break;
  case 23:
    label = "UCLK";
    break;
  case 24:
    label = "FCLK";
    break;
  case 25:
    label = "GPU Boost Limit";
    break;
  default:
    break;
  }

  if (label) {
    printf("  %-16s %u MHz\n", label, raw_value);
  } else {
    printf("  soc%-2d\t %u MHz\n", index, raw_value);
  }
}

static const char *
soc_power_rail_name(int lane) {
  switch (lane) {
  case 0:
    return "GPU Core";
  case 1:
    return "GPU IO";
  case 2:
    return "CPU + SoC";
  case 3:
    return "CPU IO";
  case 4:
    return "GDDR6 Ch 0-1";
  case 5:
    return "GDDR6 Ch 2-3";
  case 6:
    return "GDDR6 Ch 4-5";
  case 7:
    return "GDDR6 Ch 6-7";
  default:
    return NULL;
  }
}

static void
print_soc_power_aux(const soc_power_sample_t *sample, int aux_base) {
  const uint8_t *aux_bytes = sample->bytes + (aux_base * sizeof(uint32_t));
  uint8_t gfx_temp_byte = aux_bytes[0];
  uint8_t cpu_temp_byte = aux_bytes[1];
  uint8_t hotspot_temp_byte = aux_bytes[2];
  uint8_t peak_temp_byte = aux_bytes[3];
  uint32_t cpu_die_temp_millic = sample->u32[aux_base + 1];
  uint32_t edc_limit_raw = sample->u32[aux_base + 2];
  uint32_t peak_temp_raw = sample->u32[aux_base + 3];

  puts("SoC power aux:");
  printf("  aux0_bytes\t %u %u %u %u\n",
         aux_bytes[0],
         aux_bytes[1],
         aux_bytes[2],
         aux_bytes[3]);
  printf("  aux0 thermal?\t %u C  %u C  %u C  %u C\n",
         gfx_temp_byte,
         cpu_temp_byte,
         hotspot_temp_byte,
         peak_temp_byte);
  printf("  CPU die temp\t %.3f C (%u mC)\n",
         (double)cpu_die_temp_millic / 1000.0,
         cpu_die_temp_millic);
  printf("  Power/Current?\t %u raw\n", edc_limit_raw);
  printf("  Peak temp?\t %u C\n", peak_temp_raw);
}

static void
print_soc_power_stats(const soc_power_sample_t *sample) {
  int printed_lanes = 0;
  int aux_base = 24;

  puts("SoC power:");
  for (int lane = 0; lane < 8; ++lane) {
    int base = lane * 3;
    const char *rail_name = soc_power_rail_name(lane);
    uint32_t power_mw = sample->u32[base];
    uint32_t voltage_mv = sample->u32[base + 1];
    uint32_t current_ma = sample->u32[base + 2];

    if (!power_mw && !voltage_mv && !current_ma) {
      continue;
    }

    printf("  %-14s %.3f W  %.3f V  %.3f A\n",
           rail_name ? rail_name : "Unknown Rail",
           (double)power_mw / 1000.0,
           (double)voltage_mv / 1000.0,
           (double)current_ma / 1000.0);
    ++printed_lanes;
  }

  if (!printed_lanes) {
    puts("  N/A");
    return;
  }

  print_soc_power_aux(sample, aux_base);
}

static double
monotonic_delta_seconds(const struct timespec *start,
                        const struct timespec *end) {
  return (double)(end->tv_sec - start->tv_sec) +
         ((double)(end->tv_nsec - start->tv_nsec) / 1000000000.0);
}

static double
proc_total_seconds(const proc_stats_t *proc) {
  return (double)proc->user_cpu_usage_time.tv_sec +
         ((double)proc->user_cpu_usage_time.tv_usec / 1000000.0) +
         (double)proc->system_cpu_usage_time.tv_sec +
         ((double)proc->system_cpu_usage_time.tv_usec / 1000000.0);
}

static int
take_thread_sample(thread_sample_t *sample) {
  int32_t capacity = MAX_THREAD_SAMPLES;

  memset(sample, 0, sizeof(*sample));
  sample->thread_count = capacity;
  if (sceKernelGetCpuUsage(sample->threads, &sample->thread_count)) {
    return -1;
  }

  if (sample->thread_count <= 0 || sample->thread_count > MAX_THREAD_SAMPLES) {
    errno = EIO;
    return -1;
  }

  if (clock_gettime(CLOCK_MONOTONIC, &sample->wall_time)) {
    return -1;
  }

  return 0;
}

static int
resolve_idle_thread_ids(const thread_sample_t *sample,
                        uint32_t idle_ids[MAX_CPU_LANES]) {
  int found = 0;
  char thread_name[0x40];

  memset(idle_ids, 0, sizeof(uint32_t) * MAX_CPU_LANES);
  for (int32_t i = 0; i < sample->thread_count; ++i) {
    int lane = -1;

    memset(thread_name, 0, sizeof(thread_name));
    if (sceKernelGetThreadName(sample->threads[i].td_tid, thread_name)) {
      continue;
    }

    if (!strcmp(thread_name, "SceIdleCpuRv")) {
      if (idle_ids[13] == 0) {
        idle_ids[13] = sample->threads[i].td_tid;
        ++found;
      }
      continue;
    }

    if (sscanf(thread_name, "SceIdleCpu%d", &lane) == 1 && lane >= 0 &&
        lane < MAX_CPU_LANES && lane != 13 && idle_ids[lane] == 0) {
      idle_ids[lane] = sample->threads[i].td_tid;
      ++found;
      if (found == MAX_CPU_LANES) {
        break;
      }
    }
  }

  return found > 0 ? 0 : -1;
}

static const proc_stats_t *
find_thread_by_tid(const thread_sample_t *sample, uint32_t tid) {
  for (int32_t i = 0; i < sample->thread_count; ++i) {
    if (sample->threads[i].td_tid == tid) {
      return &sample->threads[i];
    }
  }

  return NULL;
}

static void
compute_cpu_core_loads(const thread_sample_t *prev, const thread_sample_t *cur,
                       const uint32_t idle_ids[MAX_CPU_LANES], int cpu_count,
                       double out_percent[MAX_CPU_LANES]) {
  double wall_delta = monotonic_delta_seconds(&prev->wall_time, &cur->wall_time);

  for (int cpu = 0; cpu < cpu_count; ++cpu) {
    out_percent[cpu] = -1.0;
  }

  if (wall_delta <= 0.0) {
    return;
  }

  for (int cpu = 0; cpu < cpu_count; ++cpu) {
    const proc_stats_t *prev_idle = NULL;
    const proc_stats_t *cur_idle = NULL;
    double idle_delta = 0.0;
    double idle_ratio = 0.0;

    if (idle_ids[cpu] == 0) {
      continue;
    }

    prev_idle = find_thread_by_tid(prev, idle_ids[cpu]);
    cur_idle = find_thread_by_tid(cur, idle_ids[cpu]);
    if (!prev_idle || !cur_idle) {
      continue;
    }

    idle_delta = proc_total_seconds(cur_idle) - proc_total_seconds(prev_idle);
    if (idle_delta < 0.0) {
      idle_delta = 0.0;
    }

    idle_ratio = idle_delta / wall_delta;
    if (idle_ratio > 1.0) {
      idle_ratio = 1.0;
    }

    out_percent[cpu] = (1.0 - idle_ratio) * 100.0;
  }
}

static int
sample_cpu_logical_loads(unsigned interval_sec, double out_percent[MAX_CPU_LANES],
                         int *out_cpu_count) {
  thread_sample_t *prev = NULL;
  thread_sample_t *cur = NULL;
  uint32_t idle_ids[MAX_CPU_LANES];
  int cpu_count = MAX_CPU_LANES;
  int resolved = 0;
  int rc = -1;

  prev = calloc(1, sizeof(*prev));
  cur = calloc(1, sizeof(*cur));
  if (!prev || !cur) {
    goto cleanup;
  }

  if (take_thread_sample(prev)) {
    goto cleanup;
  }

  if (resolve_idle_thread_ids(prev, idle_ids)) {
    errno = ENOENT;
    goto cleanup;
  }

  sleep(interval_sec);

  if (take_thread_sample(cur)) {
    goto cleanup;
  }

  for (int cpu = 0; cpu < cpu_count; ++cpu) {
    if (idle_ids[cpu] != 0) {
      ++resolved;
    }
  }

  if (resolved < cpu_count) {
    uint32_t cur_idle_ids[MAX_CPU_LANES];

    if (!resolve_idle_thread_ids(cur, cur_idle_ids)) {
      for (int cpu = 0; cpu < cpu_count; ++cpu) {
        if (idle_ids[cpu] == 0) {
          idle_ids[cpu] = cur_idle_ids[cpu];
        }
      }
    }
  }

  compute_cpu_core_loads(prev, cur, idle_ids, cpu_count, out_percent);
  *out_cpu_count = cpu_count;
  rc = 0;

cleanup:
  free(prev);
  free(cur);
  return rc;
}

static int
sample_and_print(unsigned interval_sec, const authid_state_t *authid_state,
                 const runtime_options_t *options) {
  char model[MAX_TEXT] = {0};
//  char serial[MAX_TEXT] = {0};
  system_sw_version_t sw_version = {0};
  int temp_c = 0;
  long cpu_freq_hz = 0;
  int cpu_core_clocks[MAX_CPU_CORES] = {0};
  uint32_t soc_clocks[26] = {0};
  soc_power_sample_t soc_power = {0};
  uint16_t fan_duty = 0;
  uint64_t fan_chassis_info = 0;
  uint64_t main_soc_id = 0;
  uint8_t usb_power_state = 0;
  uint8_t bd_power_state = 0;
  memory_stats_t ram = {0};
  memory_stats_t vram = {0};
  shellcore_state_t shellcore_state = {0};
  shellcore_vm_stats_t vm_stats = {0};
  fan_mode_debug_t fan_mode_debug = {0};
  double cpu_loads[MAX_CPU_LANES];
  int cpu_count = 0;
  const char *sw_version_text = NULL;

  if (!sceKernelGetHwModelName(model)) {
    printf("Model:\t\t %s\n", model);
  } else {
    perror("sceKernelGetHwModelName");
  }
/*
  if (!sceKernelGetHwSerialNumber(serial)) {
    printf("S/N:\t\t %s\n", serial);
  } else {
    perror("sceKernelGetHwSerialNumber");
  }
*/
  if (!sceKernelGetProsperoSystemSwVersion(&sw_version)) {
    sw_version_text = sw_version.version;
    while (sw_version_text && *sw_version_text == ' ') {
      ++sw_version_text;
    }
    if (sw_version_text && *sw_version_text) {
      printf("System Software: %s\n", sw_version_text);
    } else {
      puts("System Software:\t N/A");
    }
  } else {
    puts("System Software:\t N/A");
  }

  main_soc_id = sceKernelGetMainSocId();
  if (main_soc_id) {
    printf("Main SoC ID:\t 0x%016llx\n", (unsigned long long)main_soc_id);
  } else {
    puts("Main SoC ID:\t N/A");
  }

  if (authid_state->available) {
    printf("AUTHID:\t\t 0x%016llx\n",
           (unsigned long long)authid_state->current);
    if (authid_state->changed) {
      printf("AUTHID prev:\t 0x%016llx\n",
             (unsigned long long)authid_state->original);
    }
  } else {
    puts("AUTHID:\t\t N/A");
  }

  if (!sceKernelGetSocSensorTemperature(0, &temp_c)) {
    printf("SoC temp:\t %d C\n", temp_c);
  } else {
    perror("sceKernelGetSocSensorTemperature");
  }

  if (!sceKernelGetCpuTemperature(&temp_c)) {
    printf("CPU temp:\t %d C\n", temp_c);
  } else {
    perror("sceKernelGetCpuTemperature");
  }

  if (!sceKernelGetCurrentFanDuty(&fan_duty, &fan_chassis_info)) {
    print_fan_duty(fan_duty);
  } else {
    puts("Fan duty:\t N/A");
    puts("Fan duty raw:\t N/A");
  }

  read_fan_mode_debug(&fan_mode_debug, options->dump_fan_mode);
  print_fan_mode_summary(&fan_mode_debug,
                         options->set_fan_mode,
                         options->fan_mode_raw);

  if (options->dump_fan_mode) {
    print_fan_mode_debug(&fan_mode_debug,
                         options->set_fan_mode,
                         options->fan_mode_raw);
  }

  if (!sceKernelIccGetUSBPowerState(&usb_power_state)) {
    printf("USB power:\t %u\n", (unsigned)usb_power_state);
  } else {
    puts("USB power:\t N/A");
  }

  if (!sceKernelIccGetBDPowerState(&bd_power_state)) {
    printf("BD power:\t %u\n", (unsigned)bd_power_state);
  } else {
    puts("BD power:\t N/A");
  }

  cpu_freq_hz = sceKernelGetCpuFrequency();
  if (cpu_freq_hz > 0) {
    printf("CPU freq:\t %.2f MHz\n", (double)cpu_freq_hz / 1000000.0);
  } else {
    puts("CPU freq:\t N/A");
  }

  if (!sceKernelGetCpuCoreClock(cpu_core_clocks)) {
    puts("CPU core freq:");
    for (int cpu = 0; cpu < MAX_CPU_CORES; ++cpu) {
      if (cpu_core_clocks[cpu] > 0) {
        printf("  cpu%-2d\t %d MHz\n", cpu, cpu_core_clocks[cpu]);
      } else {
        printf("  cpu%-2d\t N/A\n", cpu);
      }
    }
  } else {
    perror("sceKernelGetCpuCoreClock");
    puts("CPU core freq:\t N/A");
  }

  if (!sceKernelGetSocClock(soc_clocks)) {
    int printed = 0;

    puts("SoC clocks:");
    for (int i = 0; i < (int)(sizeof(soc_clocks) / sizeof(soc_clocks[0])); ++i) {
      if (!soc_clocks[i]) {
        continue;
      }

      print_soc_clock_entry(i, soc_clocks[i]);
      ++printed;
    }
    if (!printed) {
      puts("  N/A");
    } 
  } else {
    puts("SoC clocks:\t N/A");
  }

  read_shellcore_state(soc_clocks, &shellcore_state);
  print_shellcore_state(&shellcore_state, main_soc_id);

  if (!sceKernelGetSocPowerConsumption(&soc_power)) {
    print_soc_power_stats(&soc_power);
  } else {
    puts("SoC power raw:\t N/A");
  }

  read_shellcore_vm_stats(&vm_stats);
  print_shellcore_vm_stats(&vm_stats);

  if (!read_system_ram_stats(&ram)) {
    print_memory_stats("System RAM", &ram, "MB");
    if (ram.approximate) {
      puts("RAM note:\t derived from vm.stats.vm.* sysctls.");
    }
  } else {
    puts("System RAM:\t N/A");
  }

  if (!read_page_table_memory_stats(1, 2, &vram)) {
    print_memory_stats("VRAM proxy", &vram, "MB");
  } else {
    puts("VRAM proxy:\t N/A");
  }

  if (sample_cpu_logical_loads(interval_sec, cpu_loads, &cpu_count)) {
    perror("sceKernelGetCpuUsage");
    puts("CPU logical load:\t N/A");
  } else {
    puts("CPU logical load:");
    for (int cpu = 0; cpu < cpu_count; ++cpu) {
      if (cpu_loads[cpu] < 0.0) {
        printf("  cpu%-2d\t N/A\n", cpu);
      } else {
        printf("  cpu%-2d\t %.1f%%\n", cpu, cpu_loads[cpu]);
      }
    }
    puts("CPU load note:\t idle-thread delta over logical CPUs.");
  }
  return 0;
}

int
main(int argc, char **argv) {
  authid_state_t authid_state;
  const bool run_sysinfo = HWINFO_BUILD_SYSINFO != 0;
  const bool run_bench = HWINFO_BUILD_BENCHMARK != 0;
  bool ran_disk_bench = !run_bench;
  runtime_options_t options = {
    .watch = HWINFO_CFG_WATCH,
    .dump_fan_mode = HWINFO_CFG_DUMP_FAN_MODE,
    .set_fan_mode = HWINFO_CFG_SET_FAN_MODE,
    .fan_mode_raw = (uint8_t)HWINFO_CFG_FAN_MODE_RAW,
    .interval_sec = HWINFO_CFG_INTERVAL_SEC,
  };

  (void)argc;
  (void)argv;

  if (ensure_process_authid(&authid_state)) {
    memset(&authid_state, 0, sizeof(authid_state));
  }

  if (run_sysinfo && options.set_fan_mode &&
      set_icc_fan_mode_raw(options.fan_mode_raw)) {
    perror("set_icc_fan_mode_raw");
    return 1;
  }

  do {
    if (run_sysinfo) {
      puts("========================================");
      if (sample_and_print(options.interval_sec, &authid_state, &options)) {
        return 1;
      }
    }

    if (!ran_disk_bench) {
      if (!run_sysinfo) {
        puts("========================================");
      }
      if (run_disk_read_benchmarks()) {
        return 1;
      }
      ran_disk_bench = true;
    }

    if (!run_sysinfo) {
      break;
    }
  } while (options.watch);

  return 0;
}
