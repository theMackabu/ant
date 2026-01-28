#include <compat.h> // IWYU pragma: keep

#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <uthash.h>
#include <utarray.h>
#include <errno.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"

#include "modules/fs.h"
#include "modules/symbol.h"

#define fs_err_code(js, code, op, path) ({ \
  jsval_t _props = js_mkobj(js); \
  js_set(js, _props, "code", js_mkstr(js, code, strlen(code))); \
  js_mkerr_props(js, JS_ERR_TYPE, _props, "%s: %s, %s '%s'", code, strerror(errno), op, path); \
})

typedef enum {
  FS_OP_READ,
  FS_OP_WRITE,
  FS_OP_UNLINK,
  FS_OP_MKDIR,
  FS_OP_RMDIR,
  FS_OP_STAT,
  FS_OP_READ_BYTES,
  FS_OP_EXISTS,
  FS_OP_READDIR,
  FS_OP_ACCESS
} fs_op_type_t;

typedef struct fs_request_s {
  struct js *js;
  jsval_t promise;
  uv_fs_t uv_req;
  fs_op_type_t op_type;
  char *path;
  char *data;
  size_t data_len;
  uv_file fd;
  int completed;
  int failed;
  char *error_msg;
} fs_request_t;

static uv_loop_t *fs_loop = NULL;
static UT_array *pending_requests = NULL;

static void free_fs_request(fs_request_t *req) {
  if (!req) return;
  
  if (req->path) free(req->path);
  if (req->data) free(req->data);
  if (req->error_msg) free(req->error_msg);
  
  uv_fs_req_cleanup(&req->uv_req);
  free(req);
}

static void remove_pending_request(fs_request_t *req) {
  if (!req || !pending_requests) return;
  
  fs_request_t **p = NULL;
  unsigned int i = 0;
  
  while ((p = (fs_request_t**)utarray_next(pending_requests, p))) {
    if (*p == req) {
      utarray_erase(pending_requests, i, 1);
      break;
    } i++;
  }
}

static void complete_request(fs_request_t *req) {
  if (req->failed) {
    const char *err_msg = req->error_msg ? req->error_msg : "Unknown error";
    jsval_t err = js_mkstr(req->js, err_msg, strlen(err_msg));
    js_reject_promise(req->js, req->promise, err);
  } else {
    jsval_t result;
    if ((req->op_type == FS_OP_READ || req->op_type == FS_OP_READ_BYTES) && req->data) {
      result = js_mkstr(req->js, req->data, req->data_len);
    } else if (req->op_type == FS_OP_STAT) {
      result = js_mkundef();
    } else result = js_mkundef();
    js_resolve_promise(req->js, req->promise, result);
  }
  
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_read_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->data_len = uv_req->result;
  
  uv_fs_t close_req;
  uv_fs_close(fs_loop, &close_req, req->fd, NULL);
  uv_fs_req_cleanup(&close_req);
  
  req->completed = 1;
  complete_request(req);
}

static void on_open_for_read(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->fd = (int)uv_req->result;
  uv_fs_req_cleanup(uv_req);
  
  uv_fs_t stat_req;
  int stat_result = uv_fs_fstat(fs_loop, &stat_req, req->fd, NULL);
  
  if (stat_result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(stat_result));
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(fs_loop, &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
  
  size_t file_size = stat_req.statbuf.st_size;
  uv_fs_req_cleanup(&stat_req);
  
  size_t alloc_size = (req->op_type == FS_OP_READ) ? file_size + 1 : file_size;
  req->data = malloc(alloc_size);
  if (!req->data) {
    req->failed = 1;
    req->error_msg = strdup("Out of memory");
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(fs_loop, &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
  
  uv_buf_t buf = uv_buf_init(req->data, (unsigned int)file_size);
  int read_result = uv_fs_read(fs_loop, uv_req, req->fd, &buf, 1, 0, on_read_complete);
  
  if (read_result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(read_result));
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(fs_loop, &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
}

static void on_write_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
  }
  
  uv_fs_t close_req;
  uv_fs_close(fs_loop, &close_req, req->fd, NULL);
  uv_fs_req_cleanup(&close_req);
  
  req->completed = 1;
  complete_request(req);
}

static void on_open_for_write(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->fd = (int)uv_req->result;
  uv_fs_req_cleanup(uv_req);
  
  uv_buf_t buf = uv_buf_init(req->data, (unsigned int)req->data_len);
  int write_result = uv_fs_write(fs_loop, uv_req, req->fd, &buf, 1, 0, on_write_complete);
  
  if (write_result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(write_result));
    req->completed = 1;
    uv_fs_t close_req;
    uv_fs_close(fs_loop, &close_req, req->fd, NULL);
    uv_fs_req_cleanup(&close_req);
    complete_request(req);
    return;
  }
}

static void on_unlink_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
  }
  
  uv_fs_req_cleanup(uv_req);
  req->completed = 1;
  complete_request(req);
}

static void on_mkdir_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
  }
  
  uv_fs_req_cleanup(uv_req);
  req->completed = 1;
  complete_request(req);
}

static void on_rmdir_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
  }
  
  uv_fs_req_cleanup(uv_req);
  req->completed = 1;
  complete_request(req);
}

static void on_stat_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  jsval_t stat_obj = js_mkobj(req->js);
  jsval_t proto = js_get_ctor_proto(req->js, "Stats", 5);
  if (is_object_type(proto)) js_set_proto(req->js, stat_obj, proto);
  
  uv_stat_t *st = &uv_req->statbuf;
  js_set_slot(req->js, stat_obj, SLOT_DATA, js_mknum((double)st->st_mode));
  js_set(req->js, stat_obj, "size", js_mknum((double)st->st_size));
  js_set(req->js, stat_obj, "mode", js_mknum((double)st->st_mode));
  js_set(req->js, stat_obj, "uid", js_mknum((double)st->st_uid));
  js_set(req->js, stat_obj, "gid", js_mknum((double)st->st_gid));
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, stat_obj);
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_exists_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  jsval_t result = (uv_req->result >= 0) ? js_mktrue() : js_mkfalse();
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, result);
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_access_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, js_mkundef());
  remove_pending_request(req);
  free_fs_request(req);
}

static void on_readdir_complete(uv_fs_t *uv_req) {
  fs_request_t *req = (fs_request_t *)uv_req->data;
  
  if (uv_req->result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror((int)uv_req->result));
    req->completed = 1;
    complete_request(req);
    return;
  }
  
  jsval_t arr = js_mkarr(req->js);
  uv_dirent_t dirent;
  
  while (uv_fs_scandir_next(uv_req, &dirent) != UV_EOF) {
    jsval_t name = js_mkstr(req->js, dirent.name, strlen(dirent.name));
    js_arr_push(req->js, arr, name);
  }
  
  req->completed = 1;
  js_resolve_promise(req->js, req->promise, arr);
  remove_pending_request(req);
  free_fs_request(req);
}

static void ensure_fs_loop(void) {
  if (!fs_loop) {
    if (rt->flags & ANT_RUNTIME_EXT_EVENT_LOOP) {
      fs_loop = uv_default_loop();
    } else {
      fs_loop = malloc(sizeof(uv_loop_t));
      uv_loop_init(fs_loop);
    }
  }
  
  if (!pending_requests) {
    utarray_new(pending_requests, &ut_ptr_icd);
  }
}

static jsval_t builtin_fs_readFileSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readFileSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readFileSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  FILE *file = fopen(path_cstr, "rb");
  if (!file) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to open file: %s", strerror(errno));
    free(path_cstr); return js_mkerr(js, "%s", err_msg);
  }
  
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  
  if (file_size < 0) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Failed to get file size");
  }
  
  char *data = malloc(file_size + 1);
  if (!data) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  size_t bytes_read = fread(data, 1, file_size, file);
  fclose(file);
  free(path_cstr);
  
  if (bytes_read != (size_t)file_size) {
    free(data);
    return js_mkerr(js, "Failed to read entire file");
  }
  
  data[file_size] = '\0';
  jsval_t result = js_mkstr(js, data, file_size);
  free(data);
  
  return result;
}

static jsval_t builtin_fs_readBytesSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readBytesSync() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readBytesSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  FILE *file = fopen(path_cstr, "rb");
  if (!file) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to open file: %s", strerror(errno));
    free(path_cstr); return js_mkerr(js, "%s", err_msg);
  }
  
  fseek(file, 0, SEEK_END);
  long file_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  
  if (file_size < 0) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Failed to get file size");
  }
  
  char *data = malloc(file_size);
  if (!data) {
    fclose(file);
    free(path_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  size_t bytes_read = fread(data, 1, file_size, file);
  fclose(file);
  free(path_cstr);
  
  if (bytes_read != (size_t)file_size) {
    free(data);
    return js_mkerr(js, "Failed to read entire file");
  }
  
  jsval_t result = js_mkstr(js, data, file_size);
  free(data);
  
  return result;
}

static jsval_t builtin_fs_readFile(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readFile() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readFile() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_READ;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_open(fs_loop, &req->uv_req, req->path, O_RDONLY, 0, on_open_for_read);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_readBytes(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readBytes() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readBytes() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_READ_BYTES;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_open(fs_loop, &req->uv_req, req->path, O_RDONLY, 0, on_open_for_read);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_writeFileSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "writeFileSync() requires path and data arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "writeFileSync() path must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "writeFileSync() data must be a string");
  
  size_t path_len, data_len;
  char *path = js_getstr(js, args[0], &path_len);
  char *data = js_getstr(js, args[1], &data_len);
  
  if (!path || !data) return js_mkerr(js, "Failed to get arguments");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  FILE *file = fopen(path_cstr, "wb");
  if (!file) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to open file: %s", strerror(errno));
    free(path_cstr); return js_mkerr(js, "%s", err_msg);
  }
  
  size_t bytes_written = fwrite(data, 1, data_len, file);
  fclose(file);
  free(path_cstr);
  
  if (bytes_written != data_len) {
    return js_mkerr(js, "Failed to write entire file");
  }
  
  return js_mkundef();
}

static jsval_t builtin_fs_copyFileSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "copyFileSync() requires src and dest arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "copyFileSync() src must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "copyFileSync() dest must be a string");
  
  size_t src_len, dest_len;
  char *src = js_getstr(js, args[0], &src_len);
  char *dest = js_getstr(js, args[1], &dest_len);
  
  if (!src || !dest) return js_mkerr(js, "Failed to get arguments");
  
  char *src_cstr = strndup(src, src_len);
  char *dest_cstr = strndup(dest, dest_len);
  if (!src_cstr || !dest_cstr) {
    free(src_cstr);
    free(dest_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  FILE *in = fopen(src_cstr, "rb");
  if (!in) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to open source file: %s", strerror(errno));
    free(src_cstr); free(dest_cstr);
    return js_mkerr(js, "%s", err_msg);  
  }
  
  FILE *out = fopen(dest_cstr, "wb");
  if (!out) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to open dest file: %s", strerror(errno));
    fclose(in);
    free(src_cstr); free(dest_cstr);
    return js_mkerr(js, "%s", err_msg);
  }
  
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
    if (fwrite(buf, 1, n, out) != n) {
      fclose(in);
      fclose(out);
      free(src_cstr);
      free(dest_cstr);
      return js_mkerr(js, "Failed to write to dest file");
    }
  }
  
  fclose(in);
  fclose(out);
  free(src_cstr);
  free(dest_cstr);
  
  return js_mkundef();
}

static jsval_t builtin_fs_renameSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "renameSync() requires oldPath and newPath arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "renameSync() oldPath must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "renameSync() newPath must be a string");
  
  size_t old_len, new_len;
  char *old_path = js_getstr(js, args[0], &old_len);
  char *new_path = js_getstr(js, args[1], &new_len);
  
  if (!old_path || !new_path) return js_mkerr(js, "Failed to get arguments");
  
  char *old_cstr = strndup(old_path, old_len);
  char *new_cstr = strndup(new_path, new_len);
  if (!old_cstr || !new_cstr) {
    free(old_cstr);
    free(new_cstr);
    return js_mkerr(js, "Out of memory");
  }
  
  int result = rename(old_cstr, new_cstr);
  free(old_cstr);
  free(new_cstr);
  
  if (result != 0) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to rename: %s", strerror(errno));
    return js_mkerr(js, "%s", err_msg);
  }
  
  return js_mkundef();
}

static jsval_t builtin_fs_appendFileSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "appendFileSync() requires path and data arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "appendFileSync() path must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "appendFileSync() data must be a string");
  
  size_t path_len, data_len;
  char *path = js_getstr(js, args[0], &path_len);
  char *data = js_getstr(js, args[1], &data_len);
  
  if (!path || !data) return js_mkerr(js, "Failed to get arguments");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  FILE *file = fopen(path_cstr, "ab");
  if (!file) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to open file: %s", strerror(errno));
    free(path_cstr); return js_mkerr(js, "%s", err_msg);
  }
  
  size_t bytes_written = fwrite(data, 1, data_len, file);
  fclose(file);
  free(path_cstr);
  
  if (bytes_written != data_len) {
    return js_mkerr(js, "Failed to write entire file");
  }
  
  return js_mkundef();
}

static jsval_t builtin_fs_writeFile(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "writeFile() requires path and data arguments");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "writeFile() path must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "writeFile() data must be a string");
  
  size_t path_len, data_len;
  char *path = js_getstr(js, args[0], &path_len);
  char *data = js_getstr(js, args[1], &data_len);
  
  if (!path || !data) return js_mkerr(js, "Failed to get arguments");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_WRITE;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->data = malloc(data_len);
  if (!req->data) {
    free(req->path);
    free(req);
    return js_mkerr(js, "Out of memory");
  }
  
  memcpy(req->data, data, data_len);
  req->data_len = data_len;
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_open(fs_loop, &req->uv_req, req->path, O_WRONLY | O_CREAT | O_TRUNC, 0644, on_open_for_write);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_unlinkSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "unlinkSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "unlinkSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  int result = unlink(path_cstr);
  free(path_cstr);
  
  if (result != 0) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to unlink file: %s", strerror(errno));
    return js_mkerr(js, "%s", err_msg);
  }
  
  return js_mkundef();
}

static jsval_t builtin_fs_unlink(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "unlink() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "unlink() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_UNLINK;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_unlink(fs_loop, &req->uv_req, req->path, on_unlink_complete);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_mkdirSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "mkdirSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "mkdirSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = 0755;
  int recursive = 0;
  
  if (nargs < 2) goto do_mkdir;
  
  switch (vtype(args[1])) {
    case T_NUM:
      mode = (int)js_getnum(args[1]);
      break;
    case T_OBJ: {
      jsval_t opt = args[1];
      recursive = js_get(js, opt, "recursive") == js_true;
      jsval_t mode_val = js_get(js, opt, "mode");
      if (vtype(mode_val) == T_NUM) mode = (int)js_getnum(mode_val);
      break;
    }
  }
  
do_mkdir:
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
#ifdef _WIN32
  (void)mode;
  int result = _mkdir(path_cstr);
#else
  int result = mkdir(path_cstr, (mode_t)mode);
#endif
  free(path_cstr);
  
  if (result != 0) {
    if (recursive && errno == EEXIST) {
      return js_mkundef();
    }
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to create directory: %s", strerror(errno));
    return js_mkerr(js, "%s", err_msg);
  }
  
  return js_mkundef();
}

static jsval_t builtin_fs_mkdir(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "mkdir() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "mkdir() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = 0755;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    mode = (int)js_getnum(args[1]);
  }
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_MKDIR;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_mkdir(fs_loop, &req->uv_req, req->path, mode, on_mkdir_complete);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_rmdirSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "rmdirSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "rmdirSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
#ifdef _WIN32
  int result = _rmdir(path_cstr);
#else
  int result = rmdir(path_cstr);
#endif
  free(path_cstr);
  
  if (result != 0) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to remove directory: %s", strerror(errno));
    return js_mkerr(js, "%s", err_msg);
  }
  
  return js_mkundef();
}

static jsval_t builtin_fs_rmdir(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "rmdir() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "rmdir() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_RMDIR;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_rmdir(fs_loop, &req->uv_req, req->path, on_rmdir_complete);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t stat_isFile(struct js *js, jsval_t *args, int nargs) {
  jsval_t this = js_getthis(js);
  jsval_t mode_val = js_get_slot(js, this, SLOT_DATA);
  
  if (vtype(mode_val) != T_NUM) return js_mkfalse();
  mode_t mode = (mode_t)js_getnum(mode_val);
  
  return S_ISREG(mode) ? js_mktrue() : js_mkfalse();
}

static jsval_t stat_isDirectory(struct js *js, jsval_t *args, int nargs) {
  jsval_t this = js_getthis(js);
  jsval_t mode_val = js_get_slot(js, this, SLOT_DATA);
  
  if (vtype(mode_val) != T_NUM) return js_mkfalse();
  mode_t mode = (mode_t)js_getnum(mode_val);
  
  return S_ISDIR(mode) ? js_mktrue() : js_mkfalse();
}

static jsval_t stat_isSymbolicLink(struct js *js, jsval_t *args, int nargs) {
  jsval_t this = js_getthis(js);
  jsval_t mode_val = js_get_slot(js, this, SLOT_DATA);
  
  if (vtype(mode_val) != T_NUM) return js_mkfalse();
  mode_t mode = (mode_t)js_getnum(mode_val);
  
  return S_ISLNK(mode) ? js_mktrue() : js_mkfalse();
}

static jsval_t create_stats_object(struct js *js, struct stat *st) {
  jsval_t stat_obj = js_mkobj(js);
  jsval_t proto = js_get_ctor_proto(js, "Stats", 5);
  if (is_special_object(proto)) js_set_proto(js, stat_obj, proto);
  
  js_set_slot(js, stat_obj, SLOT_DATA, js_mknum((double)st->st_mode));
  js_set(js, stat_obj, "size", js_mknum((double)st->st_size));
  js_set(js, stat_obj, "mode", js_mknum((double)st->st_mode));
  js_set(js, stat_obj, "uid", js_mknum((double)st->st_uid));
  js_set(js, stat_obj, "gid", js_mknum((double)st->st_gid));
  
  return stat_obj;
}

static const char *errno_to_code(int err_num) {
  switch (err_num) {
    case ENOENT: return "ENOENT";
    case EACCES: return "EACCES";
    case ENOTDIR: return "ENOTDIR";
    case ELOOP: return "ELOOP";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case EOVERFLOW: return "EOVERFLOW";
    case EROFS: return "EROFS";
    case ETXTBSY: return "ETXTBSY";
    case EEXIST: return "EEXIST";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EISDIR: return "EISDIR";
    case EBUSY: return "EBUSY";
    case EINVAL: return "EINVAL";
    case EPERM: return "EPERM";
    case EIO: return "EIO";
    default: return "UNKNOWN";
  }
}

static jsval_t builtin_fs_statSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "statSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "statSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  struct stat st;
  int result = stat(path_cstr, &st);
  
  if (result != 0) {
    const char *code = errno_to_code(errno);
    jsval_t err = fs_err_code(js, code, "stat", path_cstr);
    free(path_cstr); return err;
  }
  
  free(path_cstr);
  return create_stats_object(js, &st);
}

static jsval_t builtin_fs_stat(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "stat() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "stat() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_STAT;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_stat(fs_loop, &req->uv_req, req->path, on_stat_complete);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_existsSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "existsSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "existsSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  struct stat st;
  int result = stat(path_cstr, &st);
  free(path_cstr);
  
  return (result == 0) ? js_mktrue() : js_mkfalse();
}

static jsval_t builtin_fs_exists(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "exists() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "exists() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_EXISTS;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_stat(fs_loop, &req->uv_req, req->path, on_exists_complete);
  
  if (result < 0) {
    req->completed = 1;
    js_resolve_promise(req->js, req->promise, js_mkfalse());
    remove_pending_request(req);
    free_fs_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_accessSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "accessSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "accessSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = F_OK;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    mode = (int)js_getnum(args[1]);
  }
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  int result = access(path_cstr, mode);
  
  if (result != 0) {
    const char *code = errno_to_code(errno);
    jsval_t err = fs_err_code(js, code, "access", path_cstr);
    free(path_cstr); return err;
  }
  
  free(path_cstr);
  return js_mkundef();
}

static jsval_t builtin_fs_access(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "access() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "access() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  int mode = F_OK;
  if (nargs >= 2 && vtype(args[1]) == T_NUM) {
    mode = (int)js_getnum(args[1]);
  }
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_ACCESS;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_access(fs_loop, &req->uv_req, req->path, mode, on_access_complete);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

static jsval_t builtin_fs_readdirSync(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readdirSync() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readdirSync() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  char *path_cstr = strndup(path, path_len);
  if (!path_cstr) return js_mkerr(js, "Out of memory");
  
  uv_fs_t req;
  int result = uv_fs_scandir(NULL, &req, path_cstr, 0, NULL);
  free(path_cstr);
  
  if (result < 0) {
    char err_msg[256];
    snprintf(err_msg, sizeof(err_msg), "Failed to read directory: %s", uv_strerror(result));
    uv_fs_req_cleanup(&req); return js_mkerr(js, "%s", err_msg);
  }
  
  jsval_t arr = js_mkarr(js);
  uv_dirent_t dirent;
  
  while (uv_fs_scandir_next(&req, &dirent) != UV_EOF) {
    jsval_t name = js_mkstr(js, dirent.name, strlen(dirent.name));
    js_arr_push(js, arr, name);
  }
  
  uv_fs_req_cleanup(&req);
  return arr;
}

static jsval_t builtin_fs_readdir(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "readdir() requires a path argument");
  
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "readdir() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ensure_fs_loop();
  
  fs_request_t *req = calloc(1, sizeof(fs_request_t));
  if (!req) return js_mkerr(js, "Out of memory");
  
  req->js = js;
  req->op_type = FS_OP_READDIR;
  req->promise = js_mkpromise(js);
  req->path = strndup(path, path_len);
  req->uv_req.data = req;
  
  utarray_push_back(pending_requests, &req);
  int result = uv_fs_scandir(fs_loop, &req->uv_req, req->path, 0, on_readdir_complete);
  
  if (result < 0) {
    req->failed = 1;
    req->error_msg = strdup(uv_strerror(result));
    req->completed = 1;
    complete_request(req);
  }
  
  return req->promise;
}

void init_fs_module(void) {
  struct js *js = rt->js;
  jsval_t glob = js_glob(js);
  
  jsval_t stats_ctor = js_mkobj(js);
  jsval_t stats_proto = js_mkobj(js);
  
  js_set(js, stats_proto, "isFile", js_mkfun(stat_isFile));
  js_set(js, stats_proto, "isDirectory", js_mkfun(stat_isDirectory));
  js_set(js, stats_proto, "isSymbolicLink", js_mkfun(stat_isSymbolicLink));
  js_set(js, stats_proto, get_toStringTag_sym_key(), js_mkstr(js, "Stats", 5));
  
  js_mkprop_fast(js, stats_ctor, "prototype", 9, stats_proto);
  js_mkprop_fast(js, stats_ctor, "name", 4, js_mkstr(js, "Stats", 5));
  js_set_descriptor(js, stats_ctor, "name", 4, 0);
  
  js_set(js, glob, "Stats", js_obj_to_func(stats_ctor));
}

jsval_t fs_library(struct js *js) {
  jsval_t lib = js_mkobj(js);
  
  js_set(js, lib, "readFile", js_mkfun(builtin_fs_readFile));
  js_set(js, lib, "readFileSync", js_mkfun(builtin_fs_readFileSync));
  js_set(js, lib, "stream", js_mkfun(builtin_fs_readBytes));
  js_set(js, lib, "open", js_mkfun(builtin_fs_readBytesSync));
  js_set(js, lib, "writeFile", js_mkfun(builtin_fs_writeFile));
  js_set(js, lib, "writeFileSync", js_mkfun(builtin_fs_writeFileSync));
  js_set(js, lib, "appendFileSync", js_mkfun(builtin_fs_appendFileSync));
  js_set(js, lib, "copyFileSync", js_mkfun(builtin_fs_copyFileSync));
  js_set(js, lib, "renameSync", js_mkfun(builtin_fs_renameSync));
  js_set(js, lib, "unlink", js_mkfun(builtin_fs_unlink));
  js_set(js, lib, "unlinkSync", js_mkfun(builtin_fs_unlinkSync));
  js_set(js, lib, "mkdir", js_mkfun(builtin_fs_mkdir));
  js_set(js, lib, "mkdirSync", js_mkfun(builtin_fs_mkdirSync));
  js_set(js, lib, "rmdir", js_mkfun(builtin_fs_rmdir));
  js_set(js, lib, "rmdirSync", js_mkfun(builtin_fs_rmdirSync));
  js_set(js, lib, "stat", js_mkfun(builtin_fs_stat));
  js_set(js, lib, "statSync", js_mkfun(builtin_fs_statSync));
  js_set(js, lib, "exists", js_mkfun(builtin_fs_exists));
  js_set(js, lib, "existsSync", js_mkfun(builtin_fs_existsSync));
  js_set(js, lib, "access", js_mkfun(builtin_fs_access));
  js_set(js, lib, "accessSync", js_mkfun(builtin_fs_accessSync));
  js_set(js, lib, "readdir", js_mkfun(builtin_fs_readdir));
  js_set(js, lib, "readdirSync", js_mkfun(builtin_fs_readdirSync));
  js_set(js, lib, get_toStringTag_sym_key(), js_mkstr(js, "fs", 2));
  
  jsval_t constants = js_mkobj(js);
  js_set(js, constants, "F_OK", js_mknum(F_OK));
  js_set(js, constants, "R_OK", js_mknum(R_OK));
  js_set(js, constants, "W_OK", js_mknum(W_OK));
  js_set(js, constants, "X_OK", js_mknum(X_OK));
  js_set(js, lib, "constants", constants);

  return lib;
}

int has_pending_fs_ops(void) {
  return (pending_requests && utarray_len(pending_requests) > 0) || (fs_loop && uv_loop_alive(fs_loop));
}

void fs_poll_events(void) {
  if (fs_loop && fs_loop == uv_default_loop() && (rt->flags & ANT_RUNTIME_EXT_EVENT_LOOP)) return;
  if (fs_loop && uv_loop_alive(fs_loop)) {
    uv_run(fs_loop, fs_loop == uv_default_loop() ? UV_RUN_NOWAIT : UV_RUN_ONCE);
    if (pending_requests && utarray_len(pending_requests) > 0) usleep(1000);
  }
}

void fs_gc_update_roots(GC_OP_VAL_ARGS) {
  if (!pending_requests) return;
  unsigned int len = utarray_len(pending_requests);
  for (unsigned int i = 0; i < len; i++) {
    fs_request_t **reqp = (fs_request_t **)utarray_eltptr(pending_requests, i);
    if (reqp && *reqp) { op_val(ctx, &(*reqp)->promise); }
  }
}
