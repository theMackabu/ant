#define _GNU_SOURCE

#include "pack.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>

extern char **environ;

static void fail(const char *msg) {
  fprintf(stderr, "ant: %s\n", msg);
  _exit(1);
}

static int make_exec_fd(void) {
  int fd = (int)syscall(SYS_memfd_create, "ant-runtime", 0);
  if (fd >= 0) return fd;

  const char *dir = getenv("TMPDIR");
  if (!dir || !dir[0]) dir = "/tmp";

  char path[4096];
  snprintf(path, sizeof(path), "%s/.ant-runtime-XXXXXX", dir);

  fd = mkstemp(path);
  if (fd < 0) fail("cannot create temporary file for runtime");
  unlink(path);
  return fd;
}

static void write_all(int fd, const unsigned char *data, size_t len) {
  size_t done = 0;
  while (done < len) {
    ssize_t put = write(fd, data + done, len - done);
    if (put <= 0) fail("failed writing runtime image");
    done += (size_t)put;
  }
}

int main(int argc, char **argv) {
  (void)argc;

  char exe[4096];
  ssize_t exe_len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
  if (exe_len <= 0) fail("cannot resolve executable path");
  exe[exe_len] = '\0';

  FILE *self = fopen(exe, "rb");
  if (!self) fail("cannot open executable");

  ant_pack_footer_t footer;
  if (fseeko(self, -(off_t)sizeof(footer), SEEK_END) != 0 ||
      fread(&footer, 1, sizeof(footer), self) != sizeof(footer) ||
      memcmp(footer.magic, ANT_PACK_MAGIC, sizeof(footer.magic)) != 0 ||
      footer.version != ANT_PACK_VERSION) {
    fail("executable is missing its packed runtime");
  }

  unsigned char *packed = malloc((size_t)footer.payload_size);
  unsigned char *image = malloc((size_t)footer.original_size);
  if (!packed || !image) fail("out of memory unpacking runtime");

  if (fseeko(self, (off_t)footer.payload_offset, SEEK_SET) != 0 ||
      fread(packed, 1, (size_t)footer.payload_size, self) != (size_t)footer.payload_size) {
    fail("failed reading packed runtime");
  }
  fclose(self);

  uLongf out_len = (uLongf)footer.original_size;
  if (uncompress(image, &out_len, packed, (uLong)footer.payload_size) != Z_OK ||
      out_len != footer.original_size) {
    fail("packed runtime is corrupt");
  }
  free(packed);

  int fd = make_exec_fd();
  write_all(fd, image, (size_t)out_len);
  free(image);

  setenv(ANT_PACK_ENV_EXE, exe, 1);

  syscall(SYS_execveat, fd, "", argv, environ, AT_EMPTY_PATH);
  fail("failed to execute unpacked runtime");
  return 1;
}
