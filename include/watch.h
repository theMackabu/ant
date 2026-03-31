#ifndef ANT_WATCH_H
#define ANT_WATCH_H

#include <stdbool.h>
#include <uv.h>

int ant_watch_start(
  uv_loop_t *loop,
  uv_fs_event_t *event,
  const char *path,
  uv_fs_event_cb callback,
  void *data,
  unsigned int flags,
  char **resolved_path_out
);

int ant_watch_run(
  int argc, char **argv, 
  const char *entry_file, bool no_clear_screen
);

void ant_watch_stop(uv_fs_event_t *event);
char *ant_watch_resolve_path(const char *path);

#endif
