#include "utils.h"
#include "config.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <argtable3.h>
#include <sys/stat.h>

static char ant_semver_buf[32];
static pthread_once_t ant_semver_once = PTHREAD_ONCE_INIT;

static const char *const js_extensions[] = {
  ".js", ".ts", 
  ".cjs", ".mjs", 
  ".jsx", ".tsx", NULL
};

static void ant_semver_init(void) {
  const char *s = ANT_VERSION;
  int d = 0, i = 0;
  while (s[i] && d < 3 && i < 31) {
    if (s[i] == '.') d++;
    ant_semver_buf[i] = s[i]; i++;
  }
  ant_semver_buf[i - (d == 3)] = '\0';
}

const char *ant_semver(void) {
  pthread_once(&ant_semver_once, ant_semver_init);
  return ant_semver_buf;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

uint64_t hash_key(const char *key, size_t len) {
  uint64_t hash = 14695981039346656037ULL;
  size_t i = 0;
  
  for (; i + 8 <= len; i += 8) {
    uint64_t word;
    memcpy(&word, key + i, 8);
    hash ^= word;
    hash *= 1099511628211ULL;
  }
  
  for (; i < len; i++) {
    hash ^= (uint8_t)key[i];
    hash *= 1099511628211ULL;
  }
  
  return hash;
}

int is_typescript_file(const char *filename) {
  if (filename == NULL) return 0;
  size_t len = strlen(filename);
  if (len < 3) return 0;
  
  const char *ext = filename + len;
  while (ext > filename && *(ext - 1) != '.' && *(ext - 1) != '/') ext--;
  if (ext == filename || *(ext - 1) != '.') return 0;
  ext--;
  
  return (strcmp(ext, ".ts") == 0 || strcmp(ext, ".mts") == 0 || strcmp(ext, ".cts") == 0);
}

static bool has_js_extension(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (!dot) return false;
  for (const char *const *ext = js_extensions; *ext; ext++) {
    if (strcmp(dot, *ext) == 0) return true;
  }
  return false;
}

char *resolve_js_file(const char *filename) {
  extern bool esm_is_url(const char *path);
  if (esm_is_url(filename)) return strdup(filename);
  
  struct stat st;
  if (stat(filename, &st) == 0 && S_ISREG(st.st_mode)) {
    return strdup(filename);
  }
  
  if (has_js_extension(filename)) return NULL;
  size_t base_len = strlen(filename);
  
  for (const char *const *ext = js_extensions; *ext; ext++) {
    size_t ext_len = strlen(*ext);
    char *test_path = try_oom(base_len + ext_len + 1);
    
    memcpy(test_path, filename, base_len);
    memcpy(test_path + base_len, *ext, ext_len + 1);
    
    if (stat(test_path, &st) == 0 && S_ISREG(st.st_mode)) {
      return test_path;
    } free(test_path);
  }
  
  return NULL;
}

int ant_version(void *argtable[]) {
  time_t build_time = (time_t)ANT_BUILD_TIMESTAMP;
  time_t now = time(NULL);
  long diff = (long)difftime(now, build_time);
  
  struct { long secs; const char *suffix; } units[] = {
    {86400, "d"}, {3600, "h"}, {60, "m"}, {1, "s"}
  };
  
  const char *suffix = "s";
  long value = diff;
  
  for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
    if (diff >= units[i].secs) {
      value = diff / units[i].secs;
      suffix = units[i].suffix; break;
    }
  }
  
  struct tm *tm = gmtime(&build_time);
  char date_buf[32];
  strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", tm);
  
  #define RED "\033[38;5;197m"
  #define RESET "\033[0m"
  
  const char *logo =
    RED
    "    ___          __         __                  _____           _       __\n"
    "   /   |  ____  / /_       / /___ __   ______ _/ ___/__________(_)___  / /_\n"
    "  / /| | / __ \\/ __/  __  / / __ `/ | / / __ `/\\__ \\/ ___/ ___/ / __ \\/ __/\n"
    " / ___ |/ / / / /_   / /_/ / /_/ /| |/ / /_/ /___/ / /__/ /  / / /_/ / /_\n"
    "/_/  |_/_/ /_/\\__/   \\____/\\__,_/ |___/\\__,_//____/\\___/_/  /_/ .___/\\__/\n"
    "                                                             /_/" RESET "   by @themackabu\n"
    RESET;
  
  fputs(logo, stdout);
  
  printf("%s (released %s, %ld%s ago)\n", 
    ANT_VERSION, 
    date_buf, 
    value, suffix
  );
  
  printf("built for %s\n", ANT_TARGET_TRIPLE);
  arg_freetable(argtable, ARGTABLE_COUNT);
  
  return EXIT_SUCCESS;
}

void *try_oom(size_t size) {
  void *p = malloc(size);
  if (!p) {
    fputs("Error: out of memory\n", stderr);
    exit(EXIT_FAILURE);
  } return p;
}

void cstr_free(cstr_buf_t *buf) {
  if (buf->heap) free(buf->heap);
}

char *cstr_init(cstr_buf_t *buf, char *stack, size_t stack_size, const char *src, size_t len) {
  if (len < stack_size) {
    buf->ptr = stack;
    buf->heap = NULL;
  } else {
    buf->heap = malloc(len + 1);
    if (!buf->heap) return NULL;
    buf->ptr = buf->heap;
  }
  memcpy(buf->ptr, src, len);
  buf->ptr[len] = '\0';
  return buf->ptr;
}