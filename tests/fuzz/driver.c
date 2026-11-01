// Standalone driver for the tests/fuzz targets when libFuzzer is not linked.
//
// Replays every file named on the command line (directories are read one
// level deep), then runs ANT_FUZZ_RUNS (default 500) mutated variants of those
// inputs from the fixed seed ANT_FUZZ_SEED (default 1). Each input is copied
// into an exactly sized allocation so ASan reports reads past its end, as it
// would under libFuzzer. A failing property aborts; rerun the printed seed to
// reproduce it; the failing input is also written to ./fuzz-failure.bin (or
// ANT_FUZZ_ARTIFACT) so it can be replayed directly or added to the corpus.
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

typedef struct {
  uint8_t *data;
  size_t size;
} input_t;

static input_t *inputs;
static size_t input_count;
static size_t input_cap;
static uint64_t rng_state;

static uint64_t rng_next(void) {
  uint64_t x = rng_state;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  return rng_state = x;
}

static size_t rng_below(size_t n) {
  return n ? (size_t)(rng_next() % n) : 0;
}

static const uint8_t *current_data;
static size_t current_size;
static const char *artifact_path = "fuzz-failure.bin";

static void save_failure(int sig) {
  int fd = open(artifact_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    for (size_t off = 0; off < current_size;) {
      ssize_t n = write(fd, current_data + off, current_size - off);
      if (n <= 0) break;
      off += (size_t)n;
    }
    close(fd);
    static const char note[] = "failing input written to ";
    (void)!write(STDERR_FILENO, note, sizeof(note) - 1);
    (void)!write(STDERR_FILENO, artifact_path, strlen(artifact_path));
    (void)!write(STDERR_FILENO, "\n", 1);
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

static void run_one(const uint8_t *data, size_t size) {
  uint8_t *copy = malloc(size ? size : 1);
  if (!copy) abort();
  if (size) memcpy(copy, data, size);
  current_data = data;
  current_size = size;
  LLVMFuzzerTestOneInput(copy, size);
  free(copy);
}

static void add_input(uint8_t *data, size_t size) {
  if (input_count == input_cap) {
    input_cap = input_cap ? input_cap * 2 : 64;
    inputs = realloc(inputs, input_cap * sizeof(*inputs));
    if (!inputs) abort();
  }
  inputs[input_count++] = (input_t){ data, size };
}

static void load_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *data = malloc(len > 0 ? (size_t)len : 1);
  if (!data || (len > 0 && fread(data, 1, (size_t)len, f) != (size_t)len)) { perror(path); exit(2); }
  fclose(f);
  add_input(data, (size_t)len);
}

static int compare_names(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

static void load_path(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) { perror(path); exit(2); }
  if (!S_ISDIR(st.st_mode)) { load_file(path); return; }

  DIR *dir = opendir(path);
  if (!dir) { perror(path); exit(2); }

  char **names = NULL;
  size_t count = 0, cap = 0;
  for (struct dirent *ent; (ent = readdir(dir));) {
    if (ent->d_name[0] == '.') continue;
    if (count == cap) {
      cap = cap ? cap * 2 : 32;
      names = realloc(names, cap * sizeof(*names));
      if (!names) abort();
    }
    size_t n = strlen(path) + strlen(ent->d_name) + 2;
    names[count] = malloc(n);
    if (!names[count]) abort();
    snprintf(names[count++], n, "%s/%s", path, ent->d_name);
  }
  closedir(dir);

  // readdir order is unspecified; sort so a seed reproduces everywhere
  qsort(names, count, sizeof(*names), compare_names);
  for (size_t i = 0; i < count; i++) {
    if (stat(names[i], &st) == 0 && S_ISREG(st.st_mode)) load_file(names[i]);
    free(names[i]);
  }
  free(names);
}

static const uint8_t interesting[] = {
  0x00, 0x01, 0x0a, 0x0d, 0x20, 0x22, 0x25, 0x27, 0x2b, 0x2d, 0x2f, 0x30,
  0x3a, 0x3d, 0x5c, 0x7b, 0x7d, 0x7f, 0x80, 0xbf, 0xc0, 0xe2, 0xed, 0xf0,
  0xf4, 0xff,
};

// One to four byte-level edits, or a splice with another input.
static size_t mutate(uint8_t *buf, size_t size, size_t cap) {
  int edits = 1 + (int)rng_below(4);
  for (int e = 0; e < edits; e++) {
    size_t pos = rng_below(size + 1);
    switch (rng_below(6)) {
      case 0:
        if (size) buf[rng_below(size)] ^= (uint8_t)(1u << rng_below(8));
        break;
      case 1:
        if (size) buf[rng_below(size)] = interesting[rng_below(sizeof(interesting))];
        break;
      case 2:
        if (size < cap) {
          memmove(buf + pos + 1, buf + pos, size - pos);
          buf[pos] = rng_below(2) ? (uint8_t)rng_next() : interesting[rng_below(sizeof(interesting))];
          size++;
        }
        break;
      case 3:
        if (size) {
          size_t at = rng_below(size);
          size_t n = 1 + rng_below(size - at);
          memmove(buf + at, buf + at + n, size - at - n);
          size -= n;
        }
        break;
      case 4:
        if (size) {
          size_t at = rng_below(size);
          size_t n = 1 + rng_below(size - at);
          if (n > cap - size) n = cap - size;
          memmove(buf + pos + n, buf + pos, size - pos);
          memmove(buf + pos, buf + (at < pos ? at : at + n), n);
          size += n;
        }
        break;
      default: {
        const input_t *other = &inputs[rng_below(input_count)];
        size_t keep = rng_below(size + 1);
        size_t from = rng_below(other->size + 1);
        size_t n = other->size - from;
        if (n > cap - keep) n = cap - keep;
        if (n) memcpy(buf + keep, other->data + from, n);
        size = keep + n;
        break;
      }
    }
  }
  return size;
}

int main(int argc, char **argv) {
  const char *artifact = getenv("ANT_FUZZ_ARTIFACT");
  if (artifact && *artifact) artifact_path = artifact;
  signal(SIGABRT, save_failure);
  signal(SIGSEGV, save_failure);
  signal(SIGBUS, save_failure);

  for (int i = 1; i < argc; i++) load_path(argv[i]);
  if (input_count == 0) add_input(NULL, 0);

  for (size_t i = 0; i < input_count; i++) run_one(inputs[i].data, inputs[i].size);

  const char *runs_env = getenv("ANT_FUZZ_RUNS");
  const char *seed_env = getenv("ANT_FUZZ_SEED");
  long runs = runs_env ? strtol(runs_env, NULL, 10) : 500;
  rng_state = seed_env ? strtoull(seed_env, NULL, 10) : 1;
  if (rng_state == 0) rng_state = 1;

  size_t max_size = 64;
  for (size_t i = 0; i < input_count; i++) if (inputs[i].size > max_size) max_size = inputs[i].size;
  size_t cap = max_size * 2;
  uint8_t *buf = malloc(cap);
  if (!buf) abort();

  for (long r = 0; r < runs; r++) {
    const input_t *base = &inputs[rng_below(input_count)];
    size_t size = base->size;
    if (size) memcpy(buf, base->data, size);
    size = mutate(buf, size, cap);
    run_one(buf, size);
  }

  free(buf);
  printf("ok: %zu inputs, %ld mutations (seed %llu)\n", input_count, runs,
    (unsigned long long)(seed_env ? strtoull(seed_env, NULL, 10) : 1));
  return 0;
}
