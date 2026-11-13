#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <uv.h>

#include "types.h"

int ant_watch_start(
  uv_loop_t *loop,
  uv_fs_event_t *event,
  const char *path,
  uv_fs_event_cb callback,
  void *data,
  unsigned int flags,
  char **resolved_path_out
);

void ant_watch_stop(uv_fs_event_t *event);
char *ant_watch_resolve_path(const char *path);

ant_watch_t *ant_watch_open(const char *entry, bool hot, bool clear_screen);
void ant_watch_close(ant_watch_t *w);

void ant_watch_attach(ant_watch_t *w, ant_t *js);
void ant_watch_detach(ant_watch_t *w, ant_t *js);

bool ant_watch_hot(const ant_watch_t *w);
bool ant_watch_wait(ant_watch_t *w);
bool ant_watch_begin_reload(ant_watch_t *w, ant_t *js);
int ant_watch_shutdown_code(ant_watch_t *w);

void ant_watch_track(ant_t *js, const char *path);
uint32_t ant_watch_generation(ant_t *js);

bool ant_watch_supervises(ant_t *js);
ant_value_t ant_watch_exit(ant_t *js);
