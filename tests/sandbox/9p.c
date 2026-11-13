#include "sandbox/host.h"
#include "shared/backend.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define T_LCREATE 14u
#define T_VERSION 100u
#define T_ATTACH 104u
#define T_WALK 110u
#define T_READ 116u
#define T_WRITE 118u
#define R_LERROR 7u
#define LINUX_EPERM 1u
#define LINUX_EINVAL 22u
#define LINUX_ELOOP 40u

typedef struct {
  unsigned char data[256];
  size_t len;
} request_t;

typedef struct {
  ant_devices_t *devices;
  ant_hvf_virtio_device_t *virtio;
} test_share_t;

static void put16(unsigned char *p, uint16_t value) {
  p[0] = (unsigned char)value;
  p[1] = (unsigned char)(value >> 8);
}

static void put32(unsigned char *p, uint32_t value) {
  put16(p, (uint16_t)value);
  put16(p + 2, (uint16_t)(value >> 16));
}

static uint32_t get32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void request_begin(request_t *req, uint8_t type) {
  memset(req, 0, sizeof(*req));
  req->data[4] = type;
  put16(req->data + 5, 7);
  req->len = 7;
}

static void request_u16(request_t *req, uint16_t value) {
  put16(req->data + req->len, value);
  req->len += 2;
}

static void request_u32(request_t *req, uint32_t value) {
  put32(req->data + req->len, value);
  req->len += 4;
}

static void request_u64(request_t *req, uint64_t value) {
  request_u32(req, (uint32_t)value);
  request_u32(req, (uint32_t)(value >> 32));
}

static void request_string(request_t *req, const char *text) {
  size_t len = strlen(text);
  request_u16(req, (uint16_t)len);
  memcpy(req->data + req->len, text, len);
  req->len += len;
}

static uint32_t send_request(test_share_t *dev, request_t *req, unsigned char *resp, size_t resp_cap) {
  put32(req->data, (uint32_t)req->len);
  return ant_hvf_9p_request(dev->virtio, req->data, req->len, resp, resp_cap);
}

static uint32_t expect_error(test_share_t *dev, request_t *req) {
  unsigned char resp[4096] = {0};
  assert(send_request(dev, req, resp, sizeof(resp)) == 11);
  assert(resp[4] == R_LERROR);
  return get32(resp + 7);
}

static void expect_ok(test_share_t *dev, request_t *req, uint32_t expected_len) {
  unsigned char resp[4096] = {0};
  assert(send_request(dev, req, resp, sizeof(resp)) == expected_len);
  assert(resp[4] == req->data[4] + 1u);
}

static void init_dev(test_share_t *dev, const char *root, bool readonly) {
  memset(dev, 0, sizeof(*dev));
  dev->devices = ant_devices_new(0, 0);
  dev->virtio = ant_devices_add_share(dev->devices, 3, 0x1000, root, "test", readonly);
  assert(dev->virtio);
  assert(dev->virtio == ant_devices_at(dev->devices, 3));

  request_t req;
  request_begin(&req, T_VERSION);
  request_u32(&req, ANT_HVF_9P_MAX_MSIZE);
  request_string(&req, "9P2000.L");
  expect_ok(dev, &req, 13 + 8);

  request_begin(&req, T_ATTACH);
  request_u32(&req, 1);
  request_u32(&req, UINT32_MAX);
  request_string(&req, "");
  request_string(&req, "");
  request_u32(&req, 0);
  expect_ok(dev, &req, 20);
}

static void walk_request(request_t *req, uint32_t newfid, const char *name) {
  request_begin(req, T_WALK);
  request_u32(req, 1);
  request_u32(req, newfid);
  request_u16(req, name ? 1 : 0);
  if (name) request_string(req, name);
}

static void walk_to(test_share_t *dev, uint32_t newfid, const char *name) {
  request_t req;
  walk_request(&req, newfid, name);
  expect_ok(dev, &req, name ? 22 : 9);
}

static void io_request(request_t *req, uint8_t type, uint32_t fid, uint64_t offset, uint32_t count) {
  request_begin(req, type);
  request_u32(req, fid);
  request_u64(req, offset);
  request_u32(req, count);
}

static void make_root(char *template, char *root) {
  char *created = mkdtemp(template);
  assert(created);
  assert(realpath(created, root));
}

static void join_path(char *out, size_t out_len, const char *dir, const char *name) {
  int written = snprintf(out, out_len, "%s/%s", dir, name);
  assert(written > 0 && (size_t)written < out_len);
}

static void test_configuration_is_validated(void) {
  ant_devices_t *devices = ant_devices_new(0, 0);
  assert(!ant_devices_add_share(devices, 3, 0x1000, NULL, "test", false));
  assert(!ant_devices_add_share(devices, 3, 0x1000, "/", NULL, false));
  assert(!ant_devices_add_share(devices, 3, 0x1000, "", "test", false));
  assert(ant_devices_at(devices, 3) == NULL);

  unsigned char resp[32];
  request_t req;
  request_begin(&req, T_ATTACH);
  put32(req.data, (uint32_t)req.len);
  assert(ant_hvf_9p_request(ant_devices_add_rng(devices, 4, 0x2000), req.data, req.len, resp, sizeof(resp)) == 0);
  assert(ant_hvf_9p_request(NULL, req.data, req.len, resp, sizeof(resp)) == 0);
  ant_devices_free(devices);
}

static void test_path_and_symlink_policy(void) {
  char root_template[] = "/tmp/ant-9p-root.XXXXXX";
  char external_template[] = "/tmp/ant-9p-external.XXXXXX";
  char root[4096];
  char external[4096];
  make_root(root_template, root);
  make_root(external_template, external);

  char link_path[4096];
  join_path(link_path, sizeof(link_path), root, "escape");
  assert(symlink(external, link_path) == 0);

  test_share_t dev;
  init_dev(&dev, root, false);

  request_t req;
  const char *bad_names[] = { "a/b", "/x", "" };
  for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
    walk_request(&req, 2, bad_names[i]);
    assert(expect_error(&dev, &req) != 0);
  }

  walk_to(&dev, 2, "escape");
  request_begin(&req, T_LCREATE);
  request_u32(&req, 2);
  request_string(&req, "new-file");
  request_u32(&req, 0);
  request_u32(&req, 0644);
  request_u32(&req, 0);
  assert(expect_error(&dev, &req) == LINUX_EPERM);

  char leaked[4096];
  join_path(leaked, sizeof(leaked), external, "new-file");
  assert(access(leaked, F_OK) != 0);

  ant_devices_free(dev.devices);
  assert(unlink(link_path) == 0);
  assert(rmdir(root) == 0);
  assert(rmdir(external) == 0);
}

static void test_tread_count_is_bounded(bool readonly) {
  enum { guard_size = 32 };
  char root_template[] = "/tmp/ant-9p-read.XXXXXX";
  char root[4096];
  make_root(root_template, root);

  char file[4096];
  join_path(file, sizeof(file), root, "payload");
  int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  assert(fd >= 0);
  assert(ftruncate(fd, ANT_HVF_9P_MAX_MSIZE + 13) == 0);
  assert(close(fd) == 0);

  test_share_t dev;
  init_dev(&dev, root, readonly);
  walk_to(&dev, 42, "payload");

  request_t req;
  io_request(&req, T_READ, 42, 0, UINT32_MAX);
  unsigned char *resp = malloc(ANT_HVF_9P_MAX_MSIZE + guard_size);
  assert(resp);
  memset(resp, 0xa5, ANT_HVF_9P_MAX_MSIZE + guard_size);

  uint32_t resp_len = send_request(&dev, &req, resp, ANT_HVF_9P_MAX_MSIZE);
  assert(resp_len == ANT_HVF_9P_MAX_MSIZE);
  assert(get32(resp) == ANT_HVF_9P_MAX_MSIZE);
  assert(resp[4] == T_READ + 1u);
  assert(get32(resp + 7) == ANT_HVF_9P_MAX_MSIZE - 11u);
  for (size_t i = ANT_HVF_9P_MAX_MSIZE; i < ANT_HVF_9P_MAX_MSIZE + guard_size; i++) {
    assert(resp[i] == 0xa5);
  }

  free(resp);
  ant_devices_free(dev.devices);
  assert(unlink(file) == 0);
  assert(rmdir(root) == 0);
}

static void test_tread_refuses_symlink(void) {
  char root_template[] = "/tmp/ant-9p-link.XXXXXX";
  char outside_template[] = "/tmp/ant-9p-outside.XXXXXX";
  char root[4096];
  char outside[4096];
  make_root(root_template, root);
  make_root(outside_template, outside);

  char secret[4096];
  join_path(secret, sizeof(secret), outside, "secret");
  FILE *fp = fopen(secret, "w");
  assert(fp);
  fputs("host secret", fp);
  fclose(fp);

  char link_path[4096];
  join_path(link_path, sizeof(link_path), root, "leak");
  assert(symlink(secret, link_path) == 0);

  test_share_t dev;
  init_dev(&dev, root, false);
  walk_to(&dev, 42, "leak");

  request_t req;
  io_request(&req, T_READ, 42, 0, 64);
  assert(expect_error(&dev, &req) == LINUX_ELOOP);

  ant_devices_free(dev.devices);
  assert(unlink(link_path) == 0);
  assert(unlink(secret) == 0);
  assert(rmdir(root) == 0);
  assert(rmdir(outside) == 0);
}

static void test_twalk_with_many_fids(void) {
  char root_template[] = "/tmp/ant-9p-walk.XXXXXX";
  char root[4096];
  make_root(root_template, root);
  char dir[4096];
  join_path(dir, sizeof(dir), root, "sub");
  assert(mkdir(dir, 0700) == 0);

  test_share_t dev;
  init_dev(&dev, root, true);
  for (uint32_t fid = 100; fid < 700; fid++) walk_to(&dev, fid, fid == 100 ? "sub" : NULL);

  request_t req;
  request_begin(&req, T_WALK);
  request_u32(&req, 100);
  request_u32(&req, 4096);
  request_u16(&req, 0);
  expect_ok(&dev, &req, 9);

  request_begin(&req, T_WALK);
  request_u32(&req, 4096);
  request_u32(&req, 4097);
  request_u16(&req, 1);
  request_string(&req, "..");
  expect_ok(&dev, &req, 22);

  ant_devices_free(dev.devices);
  assert(rmdir(dir) == 0);
  assert(rmdir(root) == 0);
}

static void test_twrite_count_overflow_is_rejected(void) {
  char root_template[] = "/tmp/ant-9p-write.XXXXXX";
  char root[4096];
  make_root(root_template, root);

  char file[4096];
  join_path(file, sizeof(file), root, "payload");
  int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0600);
  assert(fd >= 0);
  assert(close(fd) == 0);

  test_share_t dev;
  init_dev(&dev, root, false);
  walk_to(&dev, 42, "payload");

  request_t req;
  io_request(&req, T_WRITE, 42, 0, UINT32_MAX);
  assert(expect_error(&dev, &req) == LINUX_EINVAL);

  struct stat st;
  assert(stat(file, &st) == 0);
  assert(st.st_size == 0);

  io_request(&req, T_WRITE, 42, 0, 5);
  memcpy(req.data + req.len, "hello", 5);
  req.len += 5;
  expect_ok(&dev, &req, 11);
  assert(stat(file, &st) == 0);
  assert(st.st_size == 5);

  ant_devices_free(dev.devices);
  assert(unlink(file) == 0);
  assert(rmdir(root) == 0);
}

static void test_temp_write_cleanup(void) {
  ant_sandbox_launch_options_t opts;
  ant_sandbox_launch_options_init(&opts);

  char err[512] = {0};
  assert(ant_sandbox_launch_add_mount(&opts, "tmp:/tmp", false, err, sizeof(err)) == 0);
  assert(opts.temp_dir_count == 1);
  assert(opts.mount_count == 1);
  assert(!opts.mounts[0].readonly);

  char temp_dir[4096];
  snprintf(temp_dir, sizeof(temp_dir), "%s", opts.temp_dirs[0]);
  assert(access(temp_dir, F_OK) == 0);

  char nested[4096];
  join_path(nested, sizeof(nested), temp_dir, "nested");
  assert(mkdir(nested, 0700) == 0);

  char file[4096];
  join_path(file, sizeof(file), nested, "file");
  FILE *fp = fopen(file, "w");
  assert(fp);
  fputs("ok", fp);
  fclose(fp);

  ant_sandbox_launch_options_cleanup(&opts);
  assert(opts.temp_dir_count == 0);
  assert(access(temp_dir, F_OK) != 0);
  assert(errno == ENOENT);
}

int main(void) {
  test_configuration_is_validated();
  test_path_and_symlink_policy();
  test_tread_count_is_bounded(true);
  test_tread_count_is_bounded(false);
  test_tread_refuses_symlink();
  test_twalk_with_many_fids();
  test_twrite_count_overflow_is_rejected();
  test_temp_write_cleanup();
  return 0;
}
