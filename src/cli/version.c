#include "utils.h"
#include "config.h"

#include <string.h>
#include <argtable3.h>
#include <pthread.h>

static char ant_semver_buf[32];
static pthread_once_t ant_semver_once = PTHREAD_ONCE_INIT;

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