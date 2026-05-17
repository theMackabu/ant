#include "backend.h"

#if defined(__aarch64__)

uint64_t ant_hvf_9p_hash(const char *path) {
  uint64_t h = 1469598103934665603ull;
  for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
    h ^= *p;
    h *= 1099511628211ull;
  }
  return h ? h : 1;
}

void ant_hvf_9p_qid(unsigned char *out, bool dir, const char *path) {
  out[0] = dir ? P9_QTDIR : 0;
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

bool ant_hvf_9p_path_bad(const char *path) {
  if (!path || path[0] == '/') return true;
  if (strcmp(path, "..") == 0) return true;
  if (strncmp(path, "../", 3) == 0) return true;
  return strstr(path, "/../") || strstr(path, "/..");
}

int ant_hvf_9p_host_path(ant_hvf_9p_device_t *dev, const char *rel, char *out, size_t out_len) {
  if (!dev->root || ant_hvf_9p_path_bad(rel)) return -ENOENT;
  int n = rel[0] ? snprintf(out, out_len, "%s/%s", dev->root, rel)
                 : snprintf(out, out_len, "%s", dev->root);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  return 0;
}

int ant_hvf_9p_stat(ant_hvf_9p_device_t *dev, const char *rel, struct stat *st) {
  memset(st, 0, sizeof(*st));
  char host[4096];
  int rc = ant_hvf_9p_host_path(dev, rel, host, sizeof(host));
  if (rc != 0) return rc;
  if (lstat(host, st) != 0) return -errno;
  return 0;
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
  int n = base[0] ? snprintf(out, out_len, "%s/%s", base, name)
                  : snprintf(out, out_len, "%s", name);
  if (n < 0 || (size_t)n >= out_len) return -ENAMETOOLONG;
  struct stat st;
  return ant_hvf_9p_stat(dev, out, &st);
}

bool ant_hvf_9p_trace_paths(void) {
  return getenv("ANT_SANDBOX_VM_TRACE_9P_PATHS") != NULL;
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

ant_hvf_9p_fid_t *ant_hvf_9p_fid(ant_hvf_9p_device_t *dev, uint32_t fid) {
  if (fid >= ANT_HVF_9P_FID_COUNT) return NULL;
  return &dev->fids[fid];
}

uint32_t ant_hvf_9p_handle(ant_hvf_9p_device_t *dev,
                                  const unsigned char *req,
                                  size_t req_len,
                                  unsigned char *resp,
                                  size_t resp_cap) {
  if (req_len < 7) return 0;
  uint8_t type = req[4];
  uint16_t tag = ant_hvf_load16(req + 5);
  uint32_t fid;
  ant_hvf_9p_fid_t *f;

  switch (type) {
    case P9_TVERSION: {
      if (req_len < 13) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t msize = ant_hvf_load32(req + 7);
      uint16_t vlen = ant_hvf_load16(req + 11);
      if (13u + vlen > req_len || 13u + vlen > resp_cap) return ant_hvf_9p_error(resp, tag, EINVAL);
      uint32_t size = 13u + vlen;
      ant_hvf_9p_hdr(resp, size, P9_RVERSION, tag);
      ant_hvf_store32(resp + 7, msize < ANT_HVF_9P_MSIZE ? msize : ANT_HVF_9P_MSIZE);
      ant_hvf_store16(resp + 11, vlen);
      memcpy(resp + 13, req + 13, vlen);
      return size;
    }
    case P9_TATTACH:
      if (req_len < 15) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid);
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
      f = ant_hvf_9p_fid(dev, fid);
      ant_hvf_9p_fid_t *nf = ant_hvf_9p_fid(dev, newfid);
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
        if (ant_hvf_9p_trace_paths()) {
          fprintf(stderr,
                  "sandbox vm: 9p walk base=%s name=%s rc=%d next=%s\n",
                  path[0] ? path : ".",
                  name,
                  rc,
                  rc == 0 && next[0] ? next : (rc == 0 ? "." : "-"));
        }
        if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
        snprintf(path, sizeof(path), "%s", next);
        struct stat st;
        ant_hvf_9p_stat(dev, path, &st);
        if (size + 13 > resp_cap) return ant_hvf_9p_error(resp, tag, ENOSPC);
        ant_hvf_9p_qid(resp + size, S_ISDIR(st.st_mode), path);
        size += 13;
      }
      nf->active = true;
      snprintf(nf->path, sizeof(nf->path), "%s", path);
      ant_hvf_store32(resp, size);
      ant_hvf_store16(resp + 7, nwname);
      return size;
    }
    case P9_TGETATTR: {
      if (req_len < 19) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      struct stat st;
      int rc = ant_hvf_9p_stat(dev, f->path, &st);
      if (ant_hvf_9p_trace_paths()) {
        fprintf(stderr,
                "sandbox vm: 9p getattr path=%s rc=%d\n",
                f->path[0] ? f->path : ".",
                rc);
      }
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      ant_hvf_9p_hdr(resp, 143, P9_RGETATTR, tag);
      ant_hvf_store64(resp + 7, P9_GETATTR_BASIC);
      ant_hvf_9p_qid(resp + 15, S_ISDIR(st.st_mode), f->path);
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
    case P9_TLOPEN:
      if (req_len < 15) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      struct stat st;
      int open_rc = ant_hvf_9p_stat(dev, f->path, &st);
      if (ant_hvf_9p_trace_paths()) {
        fprintf(stderr,
                "sandbox vm: 9p lopen path=%s rc=%d\n",
                f->path[0] ? f->path : ".",
                open_rc);
      }
      if (open_rc != 0) return ant_hvf_9p_error(resp, tag, ENOENT);
      ant_hvf_9p_hdr(resp, 24, P9_RLOPEN, tag);
      ant_hvf_9p_qid(resp + 7, S_ISDIR(st.st_mode), f->path);
      ant_hvf_store32(resp + 20, ANT_HVF_9P_IOUNIT);
      return 24;
    case P9_TREAD: {
      if (req_len < 23) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint64_t offset = ant_hvf_load64(req + 11);
      uint32_t count = ant_hvf_load32(req + 19);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (11u + count > resp_cap) count = (uint32_t)(resp_cap - 11u);
      uint32_t got = 0;
      char host[4096];
      int rc = ant_hvf_9p_host_path(dev, f->path, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
      int fd = open(host, O_RDONLY);
      if (fd < 0) return ant_hvf_9p_error(resp, tag, (uint32_t)errno);
      ssize_t n = pread(fd, resp + 11, count, (off_t)offset);
      if (ant_hvf_9p_trace_paths()) {
        fprintf(stderr,
                "sandbox vm: 9p read path=%s offset=%llu count=%u got=%zd\n",
                f->path[0] ? f->path : ".",
                (unsigned long long)offset,
                count,
                n);
      }
      if (n < 0) {
        uint32_t e = (uint32_t)errno;
        close(fd);
        return ant_hvf_9p_error(resp, tag, e);
      }
      got = (uint32_t)n;
      close(fd);
      ant_hvf_9p_hdr(resp, 11u + got, P9_RREAD, tag);
      ant_hvf_store32(resp + 7, got);
      return 11u + got;
    }
    case P9_TREADDIR: {
      if (req_len < 23) return ant_hvf_9p_error(resp, tag, EINVAL);
      fid = ant_hvf_load32(req + 7);
      uint64_t offset = ant_hvf_load64(req + 11);
      uint32_t count = ant_hvf_load32(req + 19);
      f = ant_hvf_9p_fid(dev, fid);
      if (!f || !f->active) return ant_hvf_9p_error(resp, tag, ENOENT);
      if (count > resp_cap - 11u) count = (uint32_t)(resp_cap - 11u);

      uint32_t used = 0;
      char host[4096];
      int rc = ant_hvf_9p_host_path(dev, f->path, host, sizeof(host));
      if (rc != 0) return ant_hvf_9p_error(resp, tag, (uint32_t)-rc);
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

      if (dev->root && getenv("ANT_SANDBOX_VM_TRACE_9P_READDIR")) {
        fprintf(stderr,
                "sandbox vm: 9p readdir path=%s offset=%llu count=%u used=%u\n",
                f->path[0] ? f->path : ".",
                (unsigned long long)offset,
                count,
                used);
      }

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
        f = ant_hvf_9p_fid(dev, ant_hvf_load32(req + 7));
        if (f) memset(f, 0, sizeof(*f));
      }
      ant_hvf_9p_hdr(resp, 7, P9_RCLUNK, tag);
      return 7;
    default:
      return ant_hvf_9p_error(resp, tag, ENOSYS);
  }
}

int ant_hvf_virtio_9p_notify(ant_hvf_vm_t *vm, ant_hvf_9p_device_t *dev) {
  ant_hvf_virtio_device_t *vdev = &dev->virtio;
  ant_hvf_virtio_queue_t *q = &vdev->queues[0];
  if (!q->enabled || !q->desc || !q->avail || !q->used) return 0;

  uint64_t desc_base = q->desc;
  uint64_t avail_base = q->avail;
  uint64_t used_base = q->used;

  unsigned char idx_raw[2];
  int rc = ant_hvf_guest_read(vm, avail_base + 2, idx_raw, sizeof(idx_raw));
  if (rc != 0) return rc;
  uint16_t avail_idx = ant_hvf_load16(idx_raw);
  if (vm->trace) {
    fprintf(stderr,
            "sandbox vm: 9p tag=%s notify avail=%u last=%u desc=0x%llx used=0x%llx\n",
            dev->tag ? dev->tag : "?",
            avail_idx,
            q->last_avail,
            (unsigned long long)desc_base,
            (unsigned long long)used_base);
  }

  while (q->last_avail != avail_idx) {
    uint16_t ring_slot = q->last_avail % q->size;
    unsigned char head_raw[2];
    rc = ant_hvf_guest_read(vm, avail_base + 4u + (uint64_t)ring_slot * 2u,
                            head_raw, sizeof(head_raw));
    if (rc != 0) return rc;
    uint16_t head = ant_hvf_load16(head_raw);

    unsigned char req[ANT_HVF_9P_MSIZE];
    unsigned char resp[ANT_HVF_9P_MSIZE];
    ant_hvf_iov_t writes[8];
    size_t req_len = 0;
    size_t writes_len = 0;
    rc = ant_hvf_9p_read_chain(vm, desc_base, head, q->size,
                               req, sizeof(req), &req_len, writes, 8, &writes_len);
    if (rc != 0) {
      if (vm->trace) fprintf(stderr, "sandbox vm: 9p read chain failed %d\n", rc);
      return rc;
    }
    if (vm->trace && req_len >= 7) {
      fprintf(stderr,
              "sandbox vm: 9p tag=%s req head=%u type=%u len=%zu writes=%zu\n",
              dev->tag ? dev->tag : "?",
              head,
              req[4],
              req_len,
              writes_len);
    }
    uint32_t resp_len = ant_hvf_9p_handle(dev, req, req_len, resp, sizeof(resp));
    if (resp_len == 0) resp_len = ant_hvf_9p_error(resp, 0, EIO);
    rc = ant_hvf_9p_write_response(vm, writes, writes_len, resp, resp_len);
    if (rc != 0) {
      if (vm->trace) fprintf(stderr, "sandbox vm: 9p write response failed %d len=%u\n", rc, resp_len);
      return rc;
    }
    rc = ant_hvf_vring_add_used(vm, used_base, q->size, head, resp_len);
    if (rc != 0) return rc;
    if (vm->trace) {
      fprintf(stderr,
              "sandbox vm: 9p tag=%s complete head=%u resp_len=%u\n",
              dev->tag ? dev->tag : "?",
              head,
              resp_len);
    }
    q->last_avail++;
  }

  return ant_hvf_virtio_interrupt(vm, vdev, 0);
}

#endif
