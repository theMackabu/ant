#include "sandbox_backend/backend.h"


static uint64_t ant_hvf_9p_now_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t ant_hvf_9p_hash(const char *path) {
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    h ^= *p;
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

static const char *ant_hvf_9p_type_name(uint8_t type) {
  switch (type) {
    case P9_TVERSION: return "version";
    case P9_TATTACH: return "attach";
    case P9_TWALK: return "walk";
    case P9_TGETATTR: return "getattr";
    case P9_TLOPEN: return "lopen";
    case P9_TREAD: return "read";
    case P9_TWRITE: return "write";
    case P9_TREADDIR: return "readdir";
    case P9_TREADLINK: return "readlink";
    case P9_TLCREATE: return "lcreate";
    case P9_TMKDIR: return "mkdir";
    case P9_TMKNOD: return "mknod";
    case P9_TSYMLINK: return "symlink";
    case P9_TSETATTR: return "setattr";
    case P9_TFSYNC: return "fsync";
    case P9_TRENAMEAT: return "renameat";
    case P9_TUNLINKAT: return "unlinkat";
    case P9_TSTATFS: return "statfs";
    case P9_TCLUNK: return "clunk";
    default: return "unknown";
  }
}

static void ant_hvf_9p_copy_path_label(char *out, size_t out_len, const char *path) {
  if (!out || out_len == 0) return;
  if (!path || path[0] == '\0') {
    snprintf(out, out_len, ".");
    return;
  }
  size_t len = strlen(path);
  if (len < out_len) {
    memcpy(out, path, len + 1);
    return;
  }
  if (out_len <= 4) {
    snprintf(out, out_len, "%s", path + len - (out_len - 1));
    return;
  }
  out[0] = '.';
  out[1] = '.';
  out[2] = '.';
  size_t tail = out_len - 4;
  memcpy(out + 3, path + len - tail, tail);
  out[out_len - 1] = '\0';
}

static ant_hvf_9p_path_stat_t *ant_hvf_9p_path_stat(ant_hvf_9p_device_t *dev, const char *path) {
  if (!dev) return NULL;
  const char *key = path ? path : "";
  uint64_t h = ant_hvf_9p_hash(key);
  size_t first = (size_t)(h % ANT_HVF_9P_TOP_PATH_COUNT);
  size_t fallback = first;
  uint64_t lowest = UINT64_MAX;

  for (size_t probe = 0; probe < ANT_HVF_9P_TOP_PATH_COUNT; probe++) {
    size_t idx = (first + probe) % ANT_HVF_9P_TOP_PATH_COUNT;
    ant_hvf_9p_path_stat_t *entry = &dev->stats.paths[idx];
    if (entry->used && entry->hash == h) return entry;
    if (!entry->used) {
      entry->used = true;
      entry->hash = h;
      ant_hvf_9p_copy_path_label(entry->path, sizeof(entry->path), key);
      return entry;
    }
    if (entry->count < lowest) {
      lowest = entry->count;
      fallback = idx;
    }
  }

  ant_hvf_9p_path_stat_t *entry = &dev->stats.paths[fallback];
  memset(entry, 0, sizeof(*entry));
  entry->used = true;
  entry->hash = h;
  ant_hvf_9p_copy_path_label(entry->path, sizeof(entry->path), key);
  return entry;
}

static void ant_hvf_9p_count_path(ant_hvf_9p_device_t *dev,
                                  const char *path,
                                  bool stat_hit,
                                  bool stat_miss,
                                  bool read,
                                  bool readdir,
                                  uint64_t bytes) {
  ant_hvf_9p_path_stat_t *entry = ant_hvf_9p_path_stat(dev, path);
  if (!entry) return;
  entry->count++;
  if (stat_hit) entry->stat_hits++;
  if (stat_miss) entry->stat_misses++;
  if (read) entry->reads++;
  if (readdir) entry->readdirs++;
  entry->bytes += bytes;
}

void ant_hvf_9p_qid(unsigned char *out, bool dir, const char *path) {
  out[0] = dir ? P9_QTDIR : 0;
  ant_hvf_store32(out + 1, 0);
  ant_hvf_store64(out + 5, ant_hvf_9p_hash(path));
}

void ant_hvf_9p_qid_mode(unsigned char *out, mode_t mode, const char *path) {
  out[0] = S_ISDIR(mode) ? P9_QTDIR : (S_ISLNK(mode) ? P9_QTSYMLINK : 0);
  ant_hvf_store32(out + 1, 0);
  ant_hvf_store64(out + 5, ant_hvf_9p_hash(path));
}

void ant_hvf_9p_hdr(unsigned char *out, uint32_t size, uint8_t type, uint16_t tag) {
  ant_hvf_store32(out, size);
  out[4] = type;
  ant_hvf_store16(out + 5, tag);
}

uint32_t ant_hvf_9p_append_dirent(unsigned char *out,
                                         uint32_t off,
                                         uint32_t cap,
                                         const char *name,
                                         const char *qid_path,
                                         bool is_dir,
                                         uint64_t next_offset,
                                         uint8_t dtype) {
  size_t name_len = strlen(name);
  uint32_t rec_len = (uint32_t)(13u + 8u + 1u + 2u + name_len);
  if (name_len > UINT16_MAX || rec_len > cap - off) return 0;

  ant_hvf_9p_qid(out + off, is_dir, qid_path);
  ant_hvf_store64(out + off + 13, next_offset);
  out[off + 21] = dtype;
  ant_hvf_store16(out + off + 22, (uint16_t)name_len);
  memcpy(out + off + 24, name, name_len);
  return rec_len;
}

uint32_t ant_hvf_9p_error(unsigned char *out, uint16_t tag, uint32_t ecode) {
  ant_hvf_9p_hdr(out, 11, P9_RLERROR, tag);
  ant_hvf_store32(out + 7, ecode);
  return 11;
}

uint32_t ant_hvf_9p_minimal(unsigned char *out, uint16_t tag, uint8_t type) {
  ant_hvf_9p_hdr(out, 7, type, tag);
  return 7;
}

uint32_t ant_hvf_9p_qid_only(unsigned char *out, uint16_t tag, uint8_t type, mode_t mode, const char *path) {
  ant_hvf_9p_hdr(out, 20, type, tag);
  ant_hvf_9p_qid_mode(out + 7, mode, path);
  return 20;
}

bool ant_hvf_9p_path_bad(const char *path) {
  if (!path || path[0] == '/') return true;
  if (path[0] == '\0') return false;
  const char *p = path;
  while (*p) {
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    if (len == 0) return true;
    if (len == 1 && p[0] == '.') return true;
    if (len == 2 && p[0] == '.' && p[1] == '.') return true;
    if (!slash) break;
    p = slash + 1;
  }
  return false;
}

bool ant_hvf_9p_name_bad(const char *name) {
  return !name || name[0] == '\0' || strchr(name, '/') ||
         strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

bool ant_hvf_9p_symlink_target_bad(const char *target) {
  return !target || target[0] == '\0' || ant_hvf_9p_path_bad(target);
}

bool ant_hvf_9p_under_root(ant_hvf_9p_device_t *dev, const char *path) {
  if (!dev->root || !path) return false;
  size_t root_len = strlen(dev->root);
  if (root_len == 0) return false;
  if (root_len == 1 && dev->root[0] == '/') return path[0] == '/';
  if (strncmp(path, dev->root, root_len) != 0) return false;
  return path[root_len] == '\0' || path[root_len] == '/';
}

int ant_hvf_9p_raw_path(ant_hvf_9p_device_t *dev, const char *rel, char *out, size_t out_len) {
  if (!dev->root || ant_hvf_9p_path_bad(rel)) return -ENOENT;
  int n = rel[0] ? snprintf(out, out_len, "%s/%s", dev->root, rel)
                 : snprintf(out, out_len, "%s", dev->root);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  return 0;
}

int ant_hvf_9p_existing_path(ant_hvf_9p_device_t *dev, const char *rel, char *out, size_t out_len) {
  char raw[ANT_HVF_9P_HOST_PATH_MAX];
  int rc = ant_hvf_9p_raw_path(dev, rel, raw, sizeof(raw));
  if (rc != 0) return rc;
  char resolved[ANT_HVF_9P_HOST_PATH_MAX];
  if (!realpath(raw, resolved)) return -errno;
  if (!ant_hvf_9p_under_root(dev, resolved)) return -EPERM;
  int n = snprintf(out, out_len, "%s", resolved);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  return 0;
}

int ant_hvf_9p_host_path(ant_hvf_9p_device_t *dev, const char *rel, char *out, size_t out_len) {
  if (!rel || rel[0] == '\0') return ant_hvf_9p_existing_path(dev, "", out, out_len);
  if (ant_hvf_9p_path_bad(rel)) return -ENOENT;

  char parent[ANT_HVF_9P_PATH_MAX];
  const char *name = rel;
  const char *slash = strrchr(rel, '/');
  if (slash) {
    size_t parent_len = (size_t)(slash - rel);
    if (parent_len >= sizeof(parent)) return -ENAMETOOLONG;
    memcpy(parent, rel, parent_len);
    parent[parent_len] = '\0';
    name = slash + 1;
  } else {
    parent[0] = '\0';
  }
  if (ant_hvf_9p_name_bad(name)) return -ENOENT;

  char parent_host[ANT_HVF_9P_HOST_PATH_MAX];
  int rc = ant_hvf_9p_existing_path(dev, parent, parent_host, sizeof(parent_host));
  if (rc != 0) return rc;
  int n = snprintf(out, out_len, "%s/%s", parent_host, name);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  return 0;
}

void ant_hvf_9p_stat_cache_clear(ant_hvf_9p_device_t *dev) {
  if (!dev || !dev->stat_cache) return;
  dev->stats.stat_cache_clears++;
  for (size_t i = 0; i < dev->stat_cache_capacity; i++) {
    free(dev->stat_cache[i].path);
    free(dev->stat_cache[i].host_path);
  }
  free(dev->stat_cache);
  dev->stat_cache = NULL;
  dev->stat_cache_capacity = 0;
  dev->stat_cache_count = 0;
}

void ant_hvf_9p_file_cache_clear(ant_hvf_9p_device_t *dev) {
  if (!dev || !dev->file_cache) return;
  for (size_t i = 0; i < dev->file_cache_capacity; i++) {
    free(dev->file_cache[i].path);
    free(dev->file_cache[i].data);
  }
  free(dev->file_cache);
  dev->file_cache = NULL;
  dev->file_cache_capacity = 0;
  dev->file_cache_count = 0;
  dev->file_cache_bytes = 0;
}

int ant_hvf_9p_buffers_init(ant_hvf_9p_device_t *dev, size_t capacity) {
  if (!dev) return -EINVAL;
  if (capacity < ANT_HVF_9P_MIN_MSIZE || capacity > ANT_HVF_9P_MAX_MSIZE) return -EINVAL;
  dev->req_buf = malloc(capacity);
  dev->resp_buf = malloc(capacity);
  if (!dev->req_buf || !dev->resp_buf) {
    ant_hvf_9p_buffers_free(dev);
    return -ENOMEM;
  }
  dev->msize = capacity < ANT_HVF_9P_DEFAULT_MSIZE ? capacity : ANT_HVF_9P_DEFAULT_MSIZE;
  dev->buf_capacity = capacity;
  return 0;
}

void ant_hvf_9p_buffers_free(ant_hvf_9p_device_t *dev) {
  if (!dev) return;
  free(dev->req_buf);
  free(dev->resp_buf);
  dev->req_buf = NULL;
  dev->resp_buf = NULL;
  dev->msize = 0;
  dev->buf_capacity = 0;
}

static int ant_hvf_9p_stat_cache_grow(ant_hvf_9p_device_t *dev) {
  size_t old_capacity = dev->stat_cache_capacity;
  size_t new_capacity = old_capacity ? old_capacity * 2u : 1024u;
  ant_hvf_9p_stat_cache_entry_t *old_entries = dev->stat_cache;
  ant_hvf_9p_stat_cache_entry_t *new_entries = calloc(new_capacity, sizeof(*new_entries));
  if (!new_entries) return -ENOMEM;

  dev->stat_cache = new_entries;
  dev->stat_cache_capacity = new_capacity;
  dev->stat_cache_count = 0;
  for (size_t i = 0; i < old_capacity; i++) {
    ant_hvf_9p_stat_cache_entry_t *old = &old_entries[i];
    if (!old->occupied) continue;
    uint64_t h = ant_hvf_9p_hash(old->path);
    for (size_t probe = 0; probe < new_capacity; probe++) {
      ant_hvf_9p_stat_cache_entry_t *dst = &new_entries[(h + probe) & (new_capacity - 1u)];
      if (dst->occupied) continue;
      *dst = *old;
      dev->stat_cache_count++;
      break;
    }
  }
  free(old_entries);
  return 0;
}

static int ant_hvf_9p_file_cache_grow(ant_hvf_9p_device_t *dev) {
  size_t old_capacity = dev->file_cache_capacity;
  size_t new_capacity = old_capacity ? old_capacity * 2u : ANT_HVF_9P_FILE_CACHE_INITIAL;
  ant_hvf_9p_file_cache_entry_t *old_entries = dev->file_cache;
  ant_hvf_9p_file_cache_entry_t *new_entries = calloc(new_capacity, sizeof(*new_entries));
  if (!new_entries) return -ENOMEM;

  dev->file_cache = new_entries;
  dev->file_cache_capacity = new_capacity;
  dev->file_cache_count = 0;
  for (size_t i = 0; i < old_capacity; i++) {
    ant_hvf_9p_file_cache_entry_t *old = &old_entries[i];
    if (!old->occupied) continue;
    uint64_t h = ant_hvf_9p_hash(old->path);
    for (size_t probe = 0; probe < new_capacity; probe++) {
      ant_hvf_9p_file_cache_entry_t *dst = &new_entries[(h + probe) & (new_capacity - 1u)];
      if (dst->occupied) continue;
      *dst = *old;
      dev->file_cache_count++;
      break;
    }
  }
  free(old_entries);
  return 0;
}

static ant_hvf_9p_stat_cache_entry_t *ant_hvf_9p_stat_cache_find(ant_hvf_9p_device_t *dev, const char *rel) {
  if (!dev->stat_cache_capacity) return NULL;
  uint64_t h = ant_hvf_9p_hash(rel);
  for (size_t probe = 0; probe < dev->stat_cache_capacity; probe++) {
    ant_hvf_9p_stat_cache_entry_t *entry = &dev->stat_cache[(h + probe) & (dev->stat_cache_capacity - 1u)];
    if (!entry->occupied) return NULL;
    if (strcmp(entry->path, rel) == 0) return entry;
  }
  return NULL;
}

static ant_hvf_9p_file_cache_entry_t *ant_hvf_9p_file_cache_find(ant_hvf_9p_device_t *dev, const char *rel) {
  if (!dev->file_cache_capacity) return NULL;
  uint64_t h = ant_hvf_9p_hash(rel);
  for (size_t probe = 0; probe < dev->file_cache_capacity; probe++) {
    ant_hvf_9p_file_cache_entry_t *entry = &dev->file_cache[(h + probe) & (dev->file_cache_capacity - 1u)];
    if (!entry->occupied) return NULL;
    if (strcmp(entry->path, rel) == 0) return entry;
  }
  return NULL;
}

static bool ant_hvf_9p_copy_file_cache_entry(ant_hvf_9p_file_cache_entry_t *entry,
                                             uint64_t offset,
                                             uint32_t count,
                                             unsigned char *out,
                                             uint32_t *got) {
  *got = 0;
  if (offset >= entry->size) return true;

  size_t start = (size_t)offset;
  size_t available = entry->size - start;
  size_t n = count < available ? count : available;
  if (n) memcpy(out, entry->data + start, n);
  *got = (uint32_t)n;
  return true;
}

static bool ant_hvf_9p_read_cached_file(ant_hvf_9p_device_t *dev,
                                        const char *rel,
                                        const char *host,
                                        const struct stat *st,
                                        uint64_t offset,
                                        uint32_t count,
                                        unsigned char *out,
                                        uint32_t *got) {
  if (!dev->readonly || !S_ISREG(st->st_mode)) {
    dev->stats.file_cache_bypasses++;
    return false;
  }
  if (st->st_size < 0 || (uint64_t)st->st_size > ANT_HVF_9P_FILE_CACHE_MAX_FILE) {
    dev->stats.file_cache_bypasses++;
    return false;
  }

  ant_hvf_9p_file_cache_entry_t *entry = ant_hvf_9p_file_cache_find(dev, rel);
  if (entry) {
    dev->stats.file_cache_hits++;
    return ant_hvf_9p_copy_file_cache_entry(entry, offset, count, out, got);
  }

  size_t size = (size_t)st->st_size;
  if (size > ANT_HVF_9P_FILE_CACHE_MAX_BYTES ||
      dev->file_cache_bytes > ANT_HVF_9P_FILE_CACHE_MAX_BYTES - size) {
    dev->stats.file_cache_bypasses++;
    return false;
  }

  dev->stats.file_cache_misses++;
  if (!dev->file_cache_capacity ||
      (dev->file_cache_count + 1u) * 10u >= dev->file_cache_capacity * 7u) {
    int grow_rc = ant_hvf_9p_file_cache_grow(dev);
    if (grow_rc != 0) return false;
  }

  uint8_t *data = malloc(size ? size : 1u);
  if (!data) return false;

  int fd = open(host, O_RDONLY);
  if (fd < 0) {
    free(data);
    return false;
  }

  size_t done = 0;
  while (done < size) {
    ssize_t n = read(fd, data + done, size - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      close(fd);
      free(data);
      return false;
    }
    if (n == 0) break;
    done += (size_t)n;
  }
  close(fd);

  if (done != size) {
    free(data);
    return false;
  }

  char *path = strdup(rel);
  if (!path) {
    free(data);
    return false;
  }

  uint64_t h = ant_hvf_9p_hash(rel);
  entry = NULL;
  for (size_t probe = 0; probe < dev->file_cache_capacity; probe++) {
    ant_hvf_9p_file_cache_entry_t *candidate = &dev->file_cache[(h + probe) & (dev->file_cache_capacity - 1u)];
    if (candidate->occupied) continue;
    entry = candidate;
    break;
  }
  if (!entry) {
    free(path);
    free(data);
    return false;
  }

  entry->occupied = true;
  entry->path = path;
  entry->data = data;
  entry->size = size;
  dev->file_cache_count++;
  dev->file_cache_bytes += size;
  return ant_hvf_9p_copy_file_cache_entry(entry, offset, count, out, got);
}

static int ant_hvf_9p_stat_cached(ant_hvf_9p_device_t *dev,
                                  const char *rel,
                                  struct stat *st,
                                  char *host_out,
                                  size_t host_out_len) {
  if (ant_hvf_9p_path_bad(rel)) return -ENOENT;

  ant_hvf_9p_stat_cache_entry_t *entry = ant_hvf_9p_stat_cache_find(dev, rel);
  if (entry) {
    dev->stats.stat_hits++;
    ant_hvf_9p_count_path(dev, rel, true, false, false, false, 0);
    if (entry->rc == 0) {
      if (st) *st = entry->st;
      if (host_out) {
        int n = snprintf(host_out, host_out_len, "%s", entry->host_path);
        if (n < 0 || (size_t)n >= host_out_len) return -ENAMETOOLONG;
      }
    }
    return entry->rc;
  }

  if ((dev->stat_cache_count + 1u) * 10u >= dev->stat_cache_capacity * 7u) {
    int grow_rc = ant_hvf_9p_stat_cache_grow(dev);
    if (grow_rc != 0) return grow_rc;
  }

  char host[ANT_HVF_9P_HOST_PATH_MAX];
  int rc = ant_hvf_9p_host_path(dev, rel, host, sizeof(host));
  dev->stats.stat_misses++;
  ant_hvf_9p_count_path(dev, rel, false, true, false, false, 0);
  struct stat local_st;
  memset(&local_st, 0, sizeof(local_st));
  if (rc == 0 && lstat(host, &local_st) != 0) rc = -errno;

  uint64_t h = ant_hvf_9p_hash(rel);
  for (size_t probe = 0; probe < dev->stat_cache_capacity; probe++) {
    entry = &dev->stat_cache[(h + probe) & (dev->stat_cache_capacity - 1u)];
    if (entry->occupied) continue;
    entry->path = strdup(rel);
    entry->host_path = rc == 0 ? strdup(host) : NULL;
    if (!entry->path || (rc == 0 && !entry->host_path)) {
      free(entry->path);
      free(entry->host_path);
      memset(entry, 0, sizeof(*entry));
      return -ENOMEM;
    }
    entry->occupied = true;
    entry->rc = rc;
    entry->st = local_st;
    dev->stat_cache_count++;
    break;
  }

  if (rc == 0) {
    if (st) *st = local_st;
    if (host_out) {
      int n = snprintf(host_out, host_out_len, "%s", host);
      if (n < 0 || (size_t)n >= host_out_len) return -ENAMETOOLONG;
    }
  }
  return rc;
}

int ant_hvf_9p_stat(ant_hvf_9p_device_t *dev, const char *rel, struct stat *st) {
  memset(st, 0, sizeof(*st));
  return ant_hvf_9p_stat_cached(dev, rel, st, NULL, 0);
}

int ant_hvf_9p_child_path(ant_hvf_9p_device_t *dev,
                          const char *parent_rel,
                          const char *name,
                          char *out,
                          size_t out_len) {
  if (ant_hvf_9p_name_bad(name)) return -EINVAL;
  char parent_host[ANT_HVF_9P_HOST_PATH_MAX];
  int rc = ant_hvf_9p_existing_path(dev, parent_rel, parent_host, sizeof(parent_host));
  if (rc != 0) return rc;
  struct stat st;
  if (lstat(parent_host, &st) != 0) return -errno;
  if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
  int n = snprintf(out, out_len, "%s/%s", parent_host, name);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  return 0;
}

int ant_hvf_9p_join_rel(const char *parent, const char *name, char *out, size_t out_len) {
  int n = parent && parent[0] ? snprintf(out, out_len, "%s/%s", parent, name)
                              : snprintf(out, out_len, "%s", name);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  return ant_hvf_9p_path_bad(out) ? -EINVAL : 0;
}

static bool ant_hvf_9p_path_equal_or_child(const char *path, const char *parent, const char **suffix) {
  size_t parent_len = strlen(parent);
  if (strcmp(path, parent) == 0) {
    *suffix = "";
    return true;
  }
  if (parent_len == 0) return false;
  if (strncmp(path, parent, parent_len) == 0 && path[parent_len] == '/') {
    *suffix = path + parent_len;
    return true;
  }
  return false;
}

static void ant_hvf_9p_rewrite_fids_after_rename(ant_hvf_9p_device_t *dev,
                                                  const char *old_rel,
                                                  const char *new_rel) {
  if (!dev || !dev->fids) return;
  for (size_t i = 0; i < dev->fid_capacity; i++) {
    ant_hvf_9p_fid_t *f = &dev->fids[i];
    if (!f->active) continue;
    const char *suffix = NULL;
    if (!ant_hvf_9p_path_equal_or_child(f->path, old_rel, &suffix)) continue;
    int n = snprintf(f->path, sizeof(f->path), "%s%s", new_rel, suffix);
    if (n < 0 || (size_t)n >= sizeof(f->path)) f->active = false;
  }
}

bool ant_hvf_9p_read_string(const unsigned char *req,
                            size_t req_len,
                            size_t *off,
                            char *out,
                            size_t out_len) {
  if (*off + 2 > req_len) return false;
  uint16_t len = ant_hvf_load16(req + *off);
  *off += 2;
  if (*off + len > req_len || len >= out_len) return false;
  memcpy(out, req + *off, len);
  out[len] = '\0';
  *off += len;
  return true;
}

uint8_t ant_hvf_9p_dtype_from_mode(mode_t mode) {
  if (S_ISDIR(mode)) return DT_DIR;
  if (S_ISLNK(mode)) return DT_LNK;
  if (S_ISCHR(mode)) return DT_CHR;
  if (S_ISBLK(mode)) return DT_BLK;
  if (S_ISFIFO(mode)) return DT_FIFO;
  if (S_ISSOCK(mode)) return DT_SOCK;
  return DT_REG;
}

bool ant_hvf_9p_dtype_is_dir(uint8_t dtype) {
  return dtype == DT_DIR;
}

int ant_hvf_9p_dirent_type(ant_hvf_9p_device_t *dev,
                                  const char *rel,
                                  uint8_t host_dtype,
                                  uint8_t *dtype,
                                  bool *is_dir) {
  if (host_dtype != DT_UNKNOWN) {
    *dtype = host_dtype;
    *is_dir = ant_hvf_9p_dtype_is_dir(host_dtype);
    return 0;
  }

  struct stat st;
  int rc = ant_hvf_9p_stat(dev, rel, &st);
  if (rc != 0) return rc;
  *dtype = ant_hvf_9p_dtype_from_mode(st.st_mode);
  *is_dir = S_ISDIR(st.st_mode);
  return 0;
}

int ant_hvf_9p_walk(ant_hvf_9p_device_t *dev, const char *base, const char *name, char *out, size_t out_len) {
  if (strcmp(name, ".") == 0) {
    int n = snprintf(out, out_len, "%s", base);
    return n < 0 || (size_t)n >= out_len ? -ENAMETOOLONG : 0;
  }
  if (strcmp(name, "..") == 0) {
    if (base[0] == '\0') {
      out[0] = '\0';
      return 0;
    }
    int n = snprintf(out, out_len, "%s", base);
    if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
    char *slash = strrchr(out, '/');
    if (slash) *slash = '\0';
    else out[0] = '\0';
    struct stat st;
    return ant_hvf_9p_stat(dev, out, &st);
  }
  if (ant_hvf_9p_name_bad(name)) return -ENOENT;
  int n = base[0] ? snprintf(out, out_len, "%s/%s", base, name)
                  : snprintf(out, out_len, "%s", name);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  struct stat st;
  return ant_hvf_9p_stat(dev, out, &st);
}

int ant_hvf_9p_read_chain(ant_hvf_vm_t *vm,
                                 uint64_t desc_base,
                                 uint16_t head,
                                 unsigned queue_size,
                                 unsigned char *req,
                                 size_t req_cap,
                                 size_t *req_len,
                                 ant_hvf_iov_t *writes,
                                 size_t writes_cap,
                                 size_t *writes_len) {
  uint16_t index = head;
  *req_len = 0;
  *writes_len = 0;

  for (unsigned chain = 0; chain < queue_size; chain++) {
    ant_vring_desc_t desc;
    int rc = ant_hvf_vring_read_desc(vm, desc_base, index, &desc);
    if (rc != 0) return rc;

    if (desc.flags & ANT_VRING_DESC_F_WRITE) {
      if (*writes_len >= writes_cap) return -E2BIG;
      writes[*writes_len] = (ant_hvf_iov_t){ .addr = desc.addr, .len = desc.len };
      (*writes_len)++;
    } else {
      if (*req_len + desc.len > req_cap) return -E2BIG;
      rc = ant_hvf_guest_read(vm, desc.addr, req + *req_len, desc.len);
      if (rc != 0) return rc;
      *req_len += desc.len;
    }

    if (!(desc.flags & ANT_VRING_DESC_F_NEXT)) break;
    index = desc.next;
  }

  return 0;
}

int ant_hvf_9p_write_response(ant_hvf_vm_t *vm,
                                     const ant_hvf_iov_t *writes,
                                     size_t writes_len,
                                     const unsigned char *resp,
                                     uint32_t resp_len) {
  uint32_t done = 0;
  for (size_t i = 0; i < writes_len && done < resp_len; i++) {
    uint32_t n = writes[i].len;
    if (n > resp_len - done) n = resp_len - done;
    int rc = ant_hvf_guest_write(vm, writes[i].addr, resp + done, n);
    if (rc != 0) return rc;
    done += n;
  }
  return done == resp_len ? 0 : -ENOSPC;
}

ant_hvf_9p_fid_t *ant_hvf_9p_fid(ant_hvf_9p_device_t *dev, uint32_t fid, bool create) {
  for (size_t i = 0; i < dev->fid_count; i++) {
    if (dev->fids[i].active && dev->fids[i].fid == fid) return &dev->fids[i];
  }
  if (!create) return NULL;

  for (size_t i = 0; i < dev->fid_count; i++) {
    if (!dev->fids[i].active) {
      memset(&dev->fids[i], 0, sizeof(dev->fids[i]));
      dev->fids[i].fid = fid;
      return &dev->fids[i];
    }
  }

  size_t old_capacity = dev->fid_capacity;
  size_t new_capacity = old_capacity ? old_capacity * 2u : ANT_HVF_9P_INITIAL_FID_COUNT;
  ant_hvf_9p_fid_t *new_fids = realloc(dev->fids, new_capacity * sizeof(*new_fids));
  if (!new_fids) return NULL;
  memset(new_fids + old_capacity, 0, (new_capacity - old_capacity) * sizeof(*new_fids));
  dev->fids = new_fids;
  dev->fid_capacity = new_capacity;
  dev->fid_count = new_capacity;
  dev->fids[old_capacity].fid = fid;
  return &dev->fids[old_capacity];
}

uint32_t ant_hvf_9p_handle(ant_hvf_9p_device_t *dev,
                                  const unsigned char *req,
                                  size_t req_len,
                                  unsigned char *resp,
                                  size_t resp_cap) {
  if (req_len < 7) return 0;
  uint8_t type = req[4];
  uint16_t tag = ant_hvf_load16(req + 5);
  dev->stats.requests++;
  dev->stats.op_counts[type]++;
  uint32_t fid;
  ant_hvf_9p_fid_t *f;

  switch (type) {
    case P9_TVERSION: {
      if (req_len < 13) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t msize = ant_hvf_load32(req + 7);
      uint16_t vlen = ant_hvf_load16(req + 11);
      if (13u + vlen > req_len || 13u + vlen > resp_cap) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (msize < ANT_HVF_9P_MIN_MSIZE) msize = ANT_HVF_9P_MIN_MSIZE;
      if (msize > dev->buf_capacity) msize = (uint32_t)dev->buf_capacity;
      dev->msize = msize;
      uint32_t size = 13u + vlen;
      ant_hvf_9p_hdr(resp, size, P9_RVERSION, tag);
      ant_hvf_store32(resp + 7, (uint32_t)dev->msize);
      ant_hvf_store16(resp + 11, vlen);
      memcpy(resp + 13, req + 13, vlen);
      return size;
    }
    case P9_TATTACH:
      if (req_len < 15) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, true);
      if (!f) return ant_hvf_9p_error(resp, tag, EINVAL);
      f->active = true;
      f->path[0] = '\0';
      ant_hvf_9p_hdr(resp, 20, P9_RATTACH, tag);
      ant_hvf_9p_qid(resp + 7, true, "");
      return 20;
    case P9_TWALK: {
      if (req_len < 17) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint32_t newfid = ant_hvf_load32(req + 11);
      uint16_t nwname = ant_hvf_load16(req + 15);
      f = ant_hvf_9p_fid(dev, fid, false);
      ant_hvf_9p_fid_t *nf = ant_hvf_9p_fid(dev, newfid, true);
      if (!f || !f->active || !nf) return ant_hvf_9p_error(resp, tag, ENOENT);
      char path[ANT_HVF_9P_PATH_MAX];
      snprintf(path, sizeof(path), "%s", f->path);
      size_t off = 17;
      uint32_t size = 9;
      ant_hvf_9p_hdr(resp, 0, P9_RWALK, tag);
      ant_hvf_store16(resp + 7, 0);
      for (uint16_t i = 0; i < nwname; i++) {
        if (off + 2 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
        uint16_t nlen = ant_hvf_load16(req + off);
        off += 2;
        if (off + nlen > req_len || nlen >= ANT_HVF_9P_PATH_MAX) return ant_hvf_9p_error(resp, tag, EINVAL);
        char name[ANT_HVF_9P_PATH_MAX];
        memcpy(name, req + off, nlen);
        name[nlen] = '\0';
        off += nlen;
        char next[ANT_HVF_9P_PATH_MAX];
        int rc = ant_hvf_9p_walk(dev, path, name, next, sizeof(next));
        if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
        snprintf(path, sizeof(path), "%s", next);
        struct stat st;
        ant_hvf_9p_stat(dev, path, &st);
        if (size + 13 > resp_cap) return ant_hvf_9p_error(resp, tag, ENOSPC);
        ant_hvf_9p_qid_mode(resp + size, st.st_mode, path);
        size += 13;
      }
      nf->active = true;
      nf->fid = newfid;
      snprintf(nf->path, sizeof(nf->path), "%s", path);
      ant_hvf_store32(resp, size);
      ant_hvf_store16(resp + 7, nwname);
      return size;
    }
    case P9_TGETATTR: {
      if (req_len < 19) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      struct stat st;
      int rc = ant_hvf_9p_stat(dev, f->path, &st);
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      ant_hvf_9p_hdr(resp, 143, P9_RGETATTR, tag);
      ant_hvf_store64(resp + 7, P9_GETATTR_BASIC);
      ant_hvf_9p_qid_mode(resp + 15, st.st_mode, f->path);
      ant_hvf_store32(resp + 28, (uint32_t)st.st_mode);
      ant_hvf_store32(resp + 32, 0);
      ant_hvf_store32(resp + 36, 0);
      ant_hvf_store64(resp + 40, (uint64_t)(st.st_nlink ? st.st_nlink : 1));
      ant_hvf_store64(resp + 48, 0);
      ant_hvf_store64(resp + 56, (uint64_t)st.st_size);
      ant_hvf_store64(resp + 64, 4096);
      ant_hvf_store64(resp + 72, (uint64_t)st.st_blocks);
      ant_hvf_store64(resp + 80, (uint64_t)st.st_atime);
      ant_hvf_store64(resp + 88, 0);
      ant_hvf_store64(resp + 96, (uint64_t)st.st_mtime);
      ant_hvf_store64(resp + 104, 0);
      ant_hvf_store64(resp + 112, (uint64_t)st.st_ctime);
      ant_hvf_store64(resp + 120, 0);
      ant_hvf_store64(resp + 128, 0);
      ant_hvf_store64(resp + 136, 0);
      return 143;
    }
    case P9_TLOPEN: {
      if (req_len < 15) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      struct stat st;
      int open_rc = ant_hvf_9p_stat(dev, f->path, &st);
      if (open_rc != 0) return ant_hvf_9p_error(resp, tag, ENOENT);
      ant_hvf_9p_hdr(resp, 24, P9_RLOPEN, tag);
      ant_hvf_9p_qid_mode(resp + 7, st.st_mode, f->path);
      ant_hvf_store32(resp + 20, (uint32_t)(dev->msize - 11u));
      return 24;
    }
    case P9_TREAD: {
      if (req_len < 23) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint64_t offset = ant_hvf_load64(req + 11);
      uint32_t count = ant_hvf_load32(req + 19);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (11u + count > resp_cap) count = (uint32_t)(resp_cap - 11u);
      uint32_t got = 0;
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      struct stat st;
      int rc = ant_hvf_9p_stat_cached(dev, f->path, &st, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) return ant_hvf_9p_error(resp, tag, EISDIR);
      if (!ant_hvf_9p_read_cached_file(dev, f->path, host, &st, offset, count, resp + 11, &got)) {
        int fd = open(host, O_RDONLY);
        if (fd < 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
        ssize_t n = pread(fd, resp + 11, count, (off_t)offset);
        if (n < 0) {
          uint32_t e = (uint32_t)errno;
          close(fd);
          return ant_hvf_9p_error(resp, tag, e);
        }
        got = (uint32_t)n;
        close(fd);
      }
      dev->stats.read_count++;
      dev->stats.read_bytes += got;
      ant_hvf_9p_count_path(dev, f->path, false, false, true, false, got);
      ant_hvf_9p_hdr(resp, 11u + got, P9_RREAD, tag);
      ant_hvf_store32(resp + 7, got);
      return 11u + got;
    }
    case P9_TWRITE: {
      if (req_len < 23) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      fid = ant_hvf_load32(req + 7);
      uint64_t offset = ant_hvf_load64(req + 11);
      uint32_t count = ant_hvf_load32(req + 19);
      if (23u + count > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_existing_path(dev, f->path, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      int fd = open(host, O_WRONLY);
      if (fd < 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      ssize_t n = pwrite(fd, req + 23, count, (off_t)offset);
      if (n < 0) {
        uint32_t e = (uint32_t)errno;
        close(fd);
        return ant_hvf_9p_error(resp, tag, e);
      }
      close(fd);
      dev->stats.write_count++;
      dev->stats.write_bytes += (uint64_t)n;
      ant_hvf_9p_stat_cache_clear(dev);
      ant_hvf_9p_hdr(resp, 11, P9_RWRITE, tag);
      ant_hvf_store32(resp + 7, (uint32_t)n);
      return 11;
    }
    case P9_TLCREATE: {
      if (req_len < 21) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      size_t off = 11;
      char name[ANT_HVF_9P_PATH_MAX];
      if (!ant_hvf_9p_read_string(req, req_len, &off, name, sizeof(name))) {
        return ant_hvf_9p_error(resp, tag, EINVAL);
      }
      if (off + 12 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t mode = ant_hvf_load32(req + off + 4);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_child_path(dev, f->path, name, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      int fd = open(host, O_CREAT | O_EXCL | O_RDWR, (mode_t)(mode & 0777u));
      if (fd < 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      close(fd);
      ant_hvf_9p_stat_cache_clear(dev);
      char rel[ANT_HVF_9P_PATH_MAX];
      rc = ant_hvf_9p_join_rel(f->path, name, rel, sizeof(rel));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      snprintf(f->path, sizeof(f->path), "%s", rel);
      struct stat st;
      rc = ant_hvf_9p_stat(dev, f->path, &st);
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      ant_hvf_9p_hdr(resp, 24, P9_RLCREATE, tag);
      ant_hvf_9p_qid_mode(resp + 7, st.st_mode, f->path);
      ant_hvf_store32(resp + 20, (uint32_t)(dev->msize - 11u));
      return 24;
    }
    case P9_TMKDIR: {
      if (req_len < 17) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      size_t off = 11;
      char name[ANT_HVF_9P_PATH_MAX];
      if (!ant_hvf_9p_read_string(req, req_len, &off, name, sizeof(name))) {
        return ant_hvf_9p_error(resp, tag, EINVAL);
      }
      if (off + 8 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t mode = ant_hvf_load32(req + off);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_child_path(dev, f->path, name, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      if (mkdir(host, (mode_t)(mode & 0777u)) != 0) {
        return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      }
      ant_hvf_9p_stat_cache_clear(dev);
      char rel[ANT_HVF_9P_PATH_MAX];
      rc = ant_hvf_9p_join_rel(f->path, name, rel, sizeof(rel));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      struct stat st;
      rc = ant_hvf_9p_stat(dev, rel, &st);
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      return ant_hvf_9p_qid_only(resp, tag, P9_RMKDIR, st.st_mode, rel);
    }
    case P9_TMKNOD: {
      if (req_len < 25) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      size_t off = 11;
      char name[ANT_HVF_9P_PATH_MAX];
      if (!ant_hvf_9p_read_string(req, req_len, &off, name, sizeof(name))) {
        return ant_hvf_9p_error(resp, tag, EINVAL);
      }
      if (off + 16 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t mode = ant_hvf_load32(req + off);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_child_path(dev, f->path, name, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      int create_rc = 0;
      if (S_ISREG((mode_t)mode)) {
        int fd = open(host, O_CREAT | O_EXCL | O_WRONLY, (mode_t)(mode & 0777u));
        if (fd < 0) create_rc = -errno;
        else close(fd);
      } else if (S_ISFIFO((mode_t)mode)) {
        create_rc = mkfifo(host, (mode_t)(mode & 0777u)) == 0 ? 0 : -errno;
      } else if (S_ISDIR((mode_t)mode)) {
        create_rc = mkdir(host, (mode_t)(mode & 0777u)) == 0 ? 0 : -errno;
      } else {
        create_rc = -ENOTSUP;
      }
      if (create_rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-create_rc);
      ant_hvf_9p_stat_cache_clear(dev);
      char rel[ANT_HVF_9P_PATH_MAX];
      rc = ant_hvf_9p_join_rel(f->path, name, rel, sizeof(rel));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      struct stat st;
      rc = ant_hvf_9p_stat(dev, rel, &st);
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      return ant_hvf_9p_qid_only(resp, tag, P9_RMKNOD, st.st_mode, rel);
    }
    case P9_TSYMLINK: {
      if (req_len < 17) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      size_t off = 11;
      char name[ANT_HVF_9P_PATH_MAX];
      char target[ANT_HVF_9P_PATH_MAX];
      if (!ant_hvf_9p_read_string(req, req_len, &off, name, sizeof(name)) ||
          !ant_hvf_9p_read_string(req, req_len, &off, target, sizeof(target))) {
        return ant_hvf_9p_error(resp, tag, EINVAL);
      }
      if (off + 4 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (ant_hvf_9p_symlink_target_bad(target)) return ant_hvf_9p_error(resp, tag, EPERM);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_child_path(dev, f->path, name, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      if (symlink(target, host) != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      ant_hvf_9p_stat_cache_clear(dev);
      char rel[ANT_HVF_9P_PATH_MAX];
      rc = ant_hvf_9p_join_rel(f->path, name, rel, sizeof(rel));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      struct stat st;
      rc = ant_hvf_9p_stat(dev, rel, &st);
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      return ant_hvf_9p_qid_only(resp, tag, P9_RSYMLINK, st.st_mode, rel);
    }
    case P9_TREADLINK: {
      if (req_len < 11) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_stat_cached(dev, f->path, NULL, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      char target[ANT_HVF_9P_PATH_MAX];
      ssize_t n = readlink(host, target, sizeof(target));
      if (n < 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      if (9u + (size_t)n > resp_cap || n > UINT16_MAX) return ant_hvf_9p_error(resp, tag, ENOSPC);
      ant_hvf_9p_hdr(resp, (uint32_t)(9 + n), P9_RREADLINK, tag);
      ant_hvf_store16(resp + 7, (uint16_t)n);
      memcpy(resp + 9, target, (size_t)n);
      return (uint32_t)(9 + n);
    }
    case P9_TSETATTR: {
      if (req_len < 63) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      fid = ant_hvf_load32(req + 7);
      uint32_t valid = ant_hvf_load32(req + 11);
      uint64_t size = ant_hvf_load64(req + 27);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (valid & ~P9_SETATTR_SIZE) return ant_hvf_9p_error(resp, tag, ENOTSUP);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_existing_path(dev, f->path, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      if ((valid & P9_SETATTR_SIZE) && truncate(host, (off_t)size) != 0) {
        return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      }
      ant_hvf_9p_stat_cache_clear(dev);
      return ant_hvf_9p_minimal(resp, tag, P9_RSETATTR);
    }
    case P9_TFSYNC: {
      if (req_len < 15) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_stat_cached(dev, f->path, NULL, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      int fd = open(host, O_RDONLY);
      if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
      }
      return ant_hvf_9p_minimal(resp, tag, P9_RFSYNC);
    }
    case P9_TRENAMEAT: {
      if (req_len < 17) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      uint32_t old_dfid = ant_hvf_load32(req + 7);
      ant_hvf_9p_fid_t *old_dir = ant_hvf_9p_fid(dev, old_dfid, false);
      if (!old_dir || !old_dir->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      size_t off = 11;
      char old_name[ANT_HVF_9P_PATH_MAX];
      char new_name[ANT_HVF_9P_PATH_MAX];
      if (!ant_hvf_9p_read_string(req, req_len, &off, old_name, sizeof(old_name))) {
        return ant_hvf_9p_error(resp, tag, EINVAL);
      }
      if (off + 4 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t new_dfid = ant_hvf_load32(req + off);
      off += 4;
      ant_hvf_9p_fid_t *new_dir = ant_hvf_9p_fid(dev, new_dfid, false);
      if (!new_dir || !new_dir->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (!ant_hvf_9p_read_string(req, req_len, &off, new_name, sizeof(new_name))) {
        return ant_hvf_9p_error(resp, tag, EINVAL);
      }
      char old_host[ANT_HVF_9P_HOST_PATH_MAX];
      char new_host[ANT_HVF_9P_HOST_PATH_MAX];
      char old_rel[ANT_HVF_9P_PATH_MAX];
      char new_rel[ANT_HVF_9P_PATH_MAX];
      int rc = ant_hvf_9p_child_path(dev, old_dir->path, old_name, old_host, sizeof(old_host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      rc = ant_hvf_9p_child_path(dev, new_dir->path, new_name, new_host, sizeof(new_host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      rc = ant_hvf_9p_join_rel(old_dir->path, old_name, old_rel, sizeof(old_rel));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      rc = ant_hvf_9p_join_rel(new_dir->path, new_name, new_rel, sizeof(new_rel));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      if (rename(old_host, new_host) != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      ant_hvf_9p_rewrite_fids_after_rename(dev, old_rel, new_rel);
      ant_hvf_9p_stat_cache_clear(dev);
      return ant_hvf_9p_minimal(resp, tag, P9_RRENAMEAT);
    }
    case P9_TUNLINKAT: {
      if (req_len < 17) return ant_hvf_9p_error(resp, tag, EINVAL);
      if (dev->readonly) return ant_hvf_9p_error(resp, tag, EROFS);
      ant_hvf_9p_stat_cache_clear(dev);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      size_t off = 11;
      char name[ANT_HVF_9P_PATH_MAX];
      if (!ant_hvf_9p_read_string(req, req_len, &off, name, sizeof(name))) {
        return ant_hvf_9p_error(resp, tag, EINVAL);
      }
      if (off + 4 > req_len) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t flags = ant_hvf_load32(req + off);
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      int rc = ant_hvf_9p_child_path(dev, f->path, name, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      int unlink_rc = (flags & P9_DOTL_AT_REMOVEDIR) ? rmdir(host) : unlink(host);
      if (unlink_rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      ant_hvf_9p_stat_cache_clear(dev);
      return ant_hvf_9p_minimal(resp, tag, P9_RUNLINKAT);
    }
    case P9_TREADDIR: {
      if (req_len < 23) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint64_t offset = ant_hvf_load64(req + 11);
      uint32_t count = ant_hvf_load32(req + 19);
      f = ant_hvf_9p_fid(dev, fid, false);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (count > resp_cap - 11u) count = (uint32_t)(resp_cap - 11u);

      uint32_t used = 0;
      char host[ANT_HVF_9P_HOST_PATH_MAX];
      struct stat st;
      int rc = ant_hvf_9p_stat_cached(dev, f->path, &st, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      if (!S_ISDIR(st.st_mode)) return ant_hvf_9p_error(resp, tag, ENOTDIR);
      DIR *dir = opendir(host);
      if (!dir) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);

      uint64_t index = 0;
      struct dirent *ent;
      while ((ent = readdir(dir))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (index++ < offset) continue;

        char child[ANT_HVF_9P_PATH_MAX];
        int npath = f->path[0] ? snprintf(child, sizeof(child), "%s/%s", f->path, ent->d_name)
                               : snprintf(child, sizeof(child), "%s", ent->d_name);
        if (npath < 0 || (size_t)npath >= sizeof(child)) continue;
        uint8_t dtype = DT_UNKNOWN;
        bool is_dir = false;
        if (ant_hvf_9p_dirent_type(dev, child, ent->d_type, &dtype, &is_dir) != 0) continue;

        uint32_t added = ant_hvf_9p_append_dirent(resp + 11, used, count,
                                                  ent->d_name, child, is_dir,
                                                  index, dtype);
        if (added == 0) break;
        used += added;
      }
      closedir(dir);

      dev->stats.readdir_count++;
      dev->stats.readdir_bytes += used;
      ant_hvf_9p_count_path(dev, f->path, false, false, false, true, used);
      ant_hvf_9p_hdr(resp, 11u + used, P9_RREADDIR, tag);
      ant_hvf_store32(resp + 7, used);
      return 11u + used;
    }
    case P9_TSTATFS:
      ant_hvf_9p_hdr(resp, 61, P9_RSTATFS, tag);
      ant_hvf_store32(resp + 7, 0x01021997);
      ant_hvf_store32(resp + 11, 4096);
      for (unsigned i = 15; i < 57; i += 8) ant_hvf_store64(resp + i, 0);
      ant_hvf_store32(resp + 57, 255);
      return 61;
    case P9_TCLUNK:
      if (req_len >= 11) {
        f = ant_hvf_9p_fid(dev, ant_hvf_load32(req + 7), false);
        if (f) memset(f, 0, sizeof(*f));
      }
      ant_hvf_9p_hdr(resp, 7, P9_RCLUNK, tag);
      return 7;
    default:
      return ant_hvf_9p_error(resp, tag, ENOSYS);
  }
}

void ant_hvf_9p_report_stats(ant_hvf_vm_t *vm, ant_hvf_9p_device_t *dev, size_t index) {
  if (!vm || !vm->verbose || !dev || dev->stats.requests == 0) return;

  uint64_t avg_us = dev->stats.requests ? dev->stats.total_ns / dev->stats.requests / 1000u : 0;
  uint64_t max_us = dev->stats.max_ns / 1000u;
  ant_hvf_verbosef(vm,
                   "9p[%zu] summary tag=%s root=%s requests=%llu errors=%llu read=%llu/%lluB readdir=%llu/%lluB write=%llu/%lluB stat_cache=%llu hit/%llu miss clears=%llu file_cache=%llu hit/%llu miss/%llu bypass entries=%zu/%zu bytes=%zu avg=%lluus max=%lluus(%s)",
                   index,
                   dev->tag ? dev->tag : "",
                   dev->root ? dev->root : "",
                   (unsigned long long)dev->stats.requests,
                   (unsigned long long)dev->stats.errors,
                   (unsigned long long)dev->stats.read_count,
                   (unsigned long long)dev->stats.read_bytes,
                   (unsigned long long)dev->stats.readdir_count,
                   (unsigned long long)dev->stats.readdir_bytes,
                   (unsigned long long)dev->stats.write_count,
                   (unsigned long long)dev->stats.write_bytes,
                   (unsigned long long)dev->stats.stat_hits,
                   (unsigned long long)dev->stats.stat_misses,
                   (unsigned long long)dev->stats.stat_cache_clears,
                   (unsigned long long)dev->stats.file_cache_hits,
                   (unsigned long long)dev->stats.file_cache_misses,
                   (unsigned long long)dev->stats.file_cache_bypasses,
                   dev->file_cache_count,
                   dev->file_cache_capacity,
                   dev->file_cache_bytes,
                   (unsigned long long)avg_us,
                   (unsigned long long)max_us,
                   ant_hvf_9p_type_name(dev->stats.max_type));

  ant_hvf_verbosef(vm,
                   "9p[%zu] ops walk=%llu getattr=%llu lopen=%llu read=%llu readdir=%llu readlink=%llu clunk=%llu",
                   index,
                   (unsigned long long)dev->stats.op_counts[P9_TWALK],
                   (unsigned long long)dev->stats.op_counts[P9_TGETATTR],
                   (unsigned long long)dev->stats.op_counts[P9_TLOPEN],
                   (unsigned long long)dev->stats.op_counts[P9_TREAD],
                   (unsigned long long)dev->stats.op_counts[P9_TREADDIR],
                   (unsigned long long)dev->stats.op_counts[P9_TREADLINK],
                   (unsigned long long)dev->stats.op_counts[P9_TCLUNK]);

  ant_hvf_9p_path_stat_t *top[ANT_HVF_9P_REPORT_TOP_COUNT] = {0};
  for (size_t i = 0; i < ANT_HVF_9P_TOP_PATH_COUNT; i++) {
    ant_hvf_9p_path_stat_t *entry = &dev->stats.paths[i];
    if (!entry->used || entry->count == 0) continue;
    for (size_t slot = 0; slot < 8; slot++) {
      if (top[slot] && top[slot]->count >= entry->count) continue;
      for (size_t move = 7; move > slot; move--) top[move] = top[move - 1];
      top[slot] = entry;
      break;
    }
  }

  for (size_t i = 0; i < 8 && top[i]; i++) {
    ant_hvf_verbosef(vm,
                     "9p[%zu] hot[%zu] count=%llu stat=%llu/%llu read=%llu readdir=%llu bytes=%llu path=%s",
                     index,
                     i,
                     (unsigned long long)top[i]->count,
                     (unsigned long long)top[i]->stat_hits,
                     (unsigned long long)top[i]->stat_misses,
                     (unsigned long long)top[i]->reads,
                     (unsigned long long)top[i]->readdirs,
                     (unsigned long long)top[i]->bytes,
                     top[i]->path);
  }
}

int ant_hvf_virtio_9p_notify(ant_hvf_vm_t *vm, ant_hvf_9p_device_t *dev) {
  ant_hvf_virtio_device_t *vdev = &dev->virtio;
  ant_hvf_virtio_queue_t *q = &vdev->queues[0];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[ANT_HVF_BYTES_U16];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[ANT_HVF_BYTES_U16];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    if (!dev->req_buf || !dev->resp_buf || dev->msize == 0) return -ENOMEM;
    unsigned char *req = dev->req_buf;
    unsigned char *resp = dev->resp_buf;
    ant_hvf_iov_t writes[ANT_HVF_9P_MAX_WRITE_IOV];
    size_t req_len = 0;
    size_t writes_len = 0;
    rc = ant_hvf_9p_read_chain(vm, desc_base, head, q->size,
                               req, dev->msize, &req_len, writes, ANT_HVF_9P_MAX_WRITE_IOV, &writes_len);
    if (rc != 0) return rc;
    uint64_t start_ns = ant_hvf_9p_now_ns();
    uint32_t resp_len = ant_hvf_9p_handle(dev, req, req_len, resp, dev->msize);
    uint64_t end_ns = ant_hvf_9p_now_ns();
    if (start_ns && end_ns >= start_ns) {
      uint64_t elapsed = end_ns - start_ns;
      dev->stats.total_ns += elapsed;
      if (elapsed > dev->stats.max_ns) {
        dev->stats.max_ns = elapsed;
        dev->stats.max_type = req_len >= 5 ? req[4] : 0;
      }
    }
    if (resp_len == 0) resp_len = ant_hvf_9p_error(resp, 0, EIO);
    if (resp_len >= 5 && resp[4] == P9_RLERROR) dev->stats.errors++;
    rc = ant_hvf_9p_write_response(vm, writes, writes_len, resp, resp_len);
    if (rc != 0) return rc;
    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, resp_len);
    if (rc != 0) return rc;
    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, vdev, 0);
}

