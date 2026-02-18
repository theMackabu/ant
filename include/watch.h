#ifndef ANT_WATCH_H
#define ANT_WATCH_H

#include <stdbool.h>

int ant_watch_run(
  int argc, char **argv, 
  const char *entry_file, bool no_clear_screen
);

#endif
