#pragma once

#include <compat.h> // IWYU pragma: keep

#include "virtio.h"

#include <dirent.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#if defined(__aarch64__)

#define ANT_VIRTIO_9P_F_MOUNT_TAG 0x1u
#define ANT_VIRTIO_9P_QUEUE_SIZE 32u
#define ANT_HVF_9P_MSIZE 8192u
#define ANT_HVF_9P_IOUNIT (ANT_HVF_9P_MSIZE - 11u)
#define ANT_HVF_9P_INITIAL_FID_COUNT 256u
#define ANT_HVF_9P_PATH_MAX 1024u
#define ANT_HVF_9P_HOST_PATH_MAX 4096u
#define ANT_HVF_9P_TOP_PATH_COUNT 64u
#define ANT_HVF_9P_TOP_PATH_LEN 160u
#define ANT_HVF_9P_FILE_CACHE_MAX_FILE (256u * 1024u)
#define ANT_HVF_9P_FILE_CACHE_MAX_BYTES (16u * 1024u * 1024u)
#define ANT_HVF_9P_FILE_CACHE_INITIAL 256u

#define P9_RLERROR 7u
#define P9_TSTATFS 8u
#define P9_RSTATFS 9u
#define P9_TLOPEN 12u
#define P9_RLOPEN 13u
#define P9_TLCREATE 14u
#define P9_RLCREATE 15u
#define P9_TSYMLINK 16u
#define P9_RSYMLINK 17u
#define P9_TMKNOD 18u
#define P9_RMKNOD 19u
#define P9_TREADLINK 22u
#define P9_RREADLINK 23u
#define P9_TGETATTR 24u
#define P9_RGETATTR 25u
#define P9_TSETATTR 26u
#define P9_RSETATTR 27u
#define P9_TREADDIR 40u
#define P9_RREADDIR 41u
#define P9_TFSYNC 50u
#define P9_RFSYNC 51u
#define P9_TMKDIR 72u
#define P9_RMKDIR 73u
#define P9_TRENAMEAT 74u
#define P9_RRENAMEAT 75u
#define P9_TUNLINKAT 76u
#define P9_RUNLINKAT 77u
#define P9_TVERSION 100u
#define P9_RVERSION 101u
#define P9_TATTACH 104u
#define P9_RATTACH 105u
#define P9_TWALK 110u
#define P9_RWALK 111u
#define P9_TREAD 116u
#define P9_RREAD 117u
#define P9_TWRITE 118u
#define P9_RWRITE 119u
#define P9_TCLUNK 120u
#define P9_RCLUNK 121u
#define P9_QTSYMLINK 0x02u
#define P9_QTDIR 0x80u
#define P9_GETATTR_BASIC 0x7ffull
#define P9_SETATTR_SIZE (1u << 3)
#define P9_DOTL_AT_REMOVEDIR 0x200u

typedef struct {
  bool active;
  uint32_t fid;
  char path[ANT_HVF_9P_PATH_MAX];
} ant_hvf_9p_fid_t;

typedef struct {
  bool occupied;
  char *path;
  char *host_path;
  int rc;
  struct stat st;
} ant_hvf_9p_stat_cache_entry_t;

typedef struct {
  bool occupied;
  char *path;
  uint8_t *data;
  size_t size;
} ant_hvf_9p_file_cache_entry_t;

typedef struct {
  bool used;
  uint64_t hash;
  char path[ANT_HVF_9P_TOP_PATH_LEN];
  uint64_t count;
  uint64_t stat_hits;
  uint64_t stat_misses;
  uint64_t reads;
  uint64_t readdirs;
  uint64_t bytes;
} ant_hvf_9p_path_stat_t;

typedef struct {
  uint64_t requests;
  uint64_t errors;
  uint64_t op_counts[256];
  uint64_t stat_hits;
  uint64_t stat_misses;
  uint64_t stat_cache_clears;
  uint64_t read_count;
  uint64_t read_bytes;
  uint64_t readdir_count;
  uint64_t readdir_bytes;
  uint64_t write_count;
  uint64_t write_bytes;
  uint64_t file_cache_hits;
  uint64_t file_cache_misses;
  uint64_t file_cache_bypasses;
  uint64_t total_ns;
  uint64_t max_ns;
  uint8_t max_type;
  ant_hvf_9p_path_stat_t paths[ANT_HVF_9P_TOP_PATH_COUNT];
} ant_hvf_9p_stats_t;

typedef struct {
  ant_hvf_virtio_device_t virtio;
  const char *root;
  const char *tag;
  bool readonly;
  ant_hvf_9p_fid_t *fids;
  size_t fid_count;
  size_t fid_capacity;
  ant_hvf_9p_stat_cache_entry_t *stat_cache;
  size_t stat_cache_capacity;
  size_t stat_cache_count;
  ant_hvf_9p_file_cache_entry_t *file_cache;
  size_t file_cache_capacity;
  size_t file_cache_count;
  size_t file_cache_bytes;
  ant_hvf_9p_stats_t stats;
} ant_hvf_9p_device_t;

uint64_t ant_hvf_9p_hash(const char *path);
void ant_hvf_9p_qid(unsigned char *out, bool dir, const char *path);
void ant_hvf_9p_hdr(unsigned char *out, uint32_t size, uint8_t type, uint16_t tag);
uint32_t ant_hvf_9p_append_dirent(unsigned char *out,
                                  uint32_t off,
                                  uint32_t cap,
                                  const char *name,
                                  const char *qid_path,
                                  bool is_dir,
                                  uint64_t next_offset,
                                  uint8_t dtype);
uint32_t ant_hvf_9p_error(unsigned char *out, uint16_t tag, uint32_t ecode);
bool ant_hvf_9p_path_bad(const char *path);
int ant_hvf_9p_host_path(ant_hvf_9p_device_t *dev, const char *rel, char *out, size_t out_len);
int ant_hvf_9p_stat(ant_hvf_9p_device_t *dev, const char *rel, struct stat *st);
void ant_hvf_9p_stat_cache_clear(ant_hvf_9p_device_t *dev);
void ant_hvf_9p_file_cache_clear(ant_hvf_9p_device_t *dev);
uint8_t ant_hvf_9p_dtype_from_mode(mode_t mode);
bool ant_hvf_9p_dtype_is_dir(uint8_t dtype);
int ant_hvf_9p_dirent_type(ant_hvf_9p_device_t *dev,
                           const char *rel,
                           uint8_t host_dtype,
                           uint8_t *dtype,
                           bool *is_dir);
int ant_hvf_9p_walk(ant_hvf_9p_device_t *dev, const char *base, const char *name, char *out, size_t out_len);
int ant_hvf_9p_read_chain(ant_hvf_vm_t *vm,
                          uint64_t desc_base,
                          uint16_t head,
                          unsigned queue_size,
                          unsigned char *req,
                          size_t req_cap,
                          size_t *req_len,
                          ant_hvf_iov_t *writes,
                          size_t writes_cap,
                          size_t *writes_len);
int ant_hvf_9p_write_response(ant_hvf_vm_t *vm,
                              const ant_hvf_iov_t *writes,
                              size_t writes_len,
                              const unsigned char *resp,
                              uint32_t resp_len);
ant_hvf_9p_fid_t *ant_hvf_9p_fid(ant_hvf_9p_device_t *dev, uint32_t fid, bool create);
uint32_t ant_hvf_9p_handle(ant_hvf_9p_device_t *dev,
                           const unsigned char *req,
                           size_t req_len,
                           unsigned char *resp,
                           size_t resp_cap);
int ant_hvf_virtio_9p_notify(ant_hvf_vm_t *vm, ant_hvf_9p_device_t *dev);
void ant_hvf_9p_report_stats(ant_hvf_vm_t *vm, ant_hvf_9p_device_t *dev, size_t index);

#endif
