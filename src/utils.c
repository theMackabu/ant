#include "utils.h"
#include "config.h"

#include <string.h>
#include <stdint.h>
#include <argtable3.h>

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
  
  printf("%s", logo);
  
  printf("%s (released %s, %ld%s ago)\n", 
    ANT_VERSION, 
    date_buf, 
    value, suffix
  );
  
  printf("built for %s\n", ANT_TARGET_TRIPLE);
  arg_freetable(argtable, ARGTABLE_COUNT);
  
  return EXIT_SUCCESS;
}