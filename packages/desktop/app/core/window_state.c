#include "window_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../api/desktop_core.h"

static ant_desktop_window_id_t next_window_id = 1;
static ant_desktop_window_state_t *windows;

ant_desktop_window_state_t *ant_desktop_window_create(ant_t *js, struct ant_desktop_state *desktop,
                                                      const char *capability_manifest,
                                                      size_t capability_manifest_length, bool transparent_browser) {
  ant_desktop_window_state_t *window = calloc(1, sizeof(*window));
  if (!window) return NULL;
  window->capability_manifest = malloc(capability_manifest_length + 1);
  if (!window->capability_manifest) {
    free(window);
    return NULL;
  }
  memcpy(window->capability_manifest, capability_manifest, capability_manifest_length);
  window->capability_manifest[capability_manifest_length] = '\0';
  window->capability_manifest_length = capability_manifest_length;
  window->identifier = next_window_id++;
  window->js = js;
  window->desktop = desktop;
  window->load_promise = js_mkundef();
  window->transparent_browser = transparent_browser;
  window->next = windows;
  windows = window;
  return window;
}

void ant_desktop_window_destroy(ant_desktop_window_state_t *window) {
  if (!window) return;
  ant_desktop_window_state_t **cursor = &windows;
  while (*cursor && *cursor != window)
    cursor = &(*cursor)->next;
  if (*cursor) *cursor = window->next;
  free(window->capability_manifest);
  free(window->preload_path);
  free(window);
}

ant_desktop_window_state_t *ant_desktop_window_find(ant_desktop_window_id_t identifier) {
  for (ant_desktop_window_state_t *window = windows; window; window = window->next) {
    if (window->identifier == identifier) return window;
  }
  return NULL;
}

ant_desktop_window_state_t *ant_desktop_window_from_value(ant_t *js, ant_value_t value) {
  if (!is_object_type(value)) return NULL;
  ant_value_t identifier = js_get(js, value, "_nativeId");
  return vtype(identifier) == kTypeNumber ? ant_desktop_window_find((ant_desktop_window_id_t)js_getnum(identifier)) : NULL;
}

size_t ant_desktop_window_count(void) {
  size_t count = 0;
  for (ant_desktop_window_state_t *window = windows; window; window = window->next)
    count++;
  return count;
}

ant_desktop_window_state_t *ant_desktop_window_first(void) {
  return windows;
}

void ant_desktop_window_key(ant_desktop_window_id_t identifier, char key[32]) {
  snprintf(key, 32, "%lld", (long long)identifier);
}

ant_value_t ant_desktop_call_function(ant_t *js, ant_value_t function, ant_value_t this_value, ant_value_t *args,
                                      int nargs) {
  return sv_vm_call_explicit_this(js->vm, js, function, this_value, args, nargs);
}

ant_value_t ant_desktop_emit_window_event(ant_desktop_window_state_t *window, const char *type, const char *detail,
                                          size_t detail_length, int64_t code) {
  if (!window || !window->js) return js_mkundef();
  char key[32];
  ant_desktop_window_key(window->identifier, key);
  ant_value_t object = js_get(window->js, window->desktop->window_objects, key);
  if (!is_object_type(object)) return js_mkundef();
  ant_value_t events = js_get(window->js, object, "_events");
  if (!is_object_type(events)) return js_mkundef();
  ant_value_t listeners = js_get(window->js, events, type);
  if (!is_array_value(listeners)) return js_mkundef();

  ant_value_t event = js_mkobj(window->js);
  js_set(window->js, event, "type", js_mkstr(window->js, type, strlen(type)));
  if (detail && detail_length) { js_set(window->js, event, "detail", js_mkstr(window->js, detail, detail_length)); }
  js_set(window->js, event, "code", js_mknum((double)code));

  ant_value_t result = js_mkundef();
  ant_offset_t count = js_arr_len(window->js, listeners);
  for (ant_offset_t index = 0; index < count; index++) {
    ant_value_t listener = js_arr_get(window->js, listeners, index);
    if (!is_callable(listener)) continue;
    result = ant_desktop_call_function(window->js, listener, object, &event, 1);
    if (is_err(result)) { fprintf(stderr, "desktop %s listener failed: %s\n", type, js_str(window->js, result)); }
  }
  return result;
}

void ant_desktop_dispatch_renderer_ipc(ant_desktop_window_state_t *window, int operation, uint64_t request_id,
                                       const char *channel, size_t channel_length, const char *payload,
                                       size_t payload_length) {
  ant_t *js = window->js;
  ant_value_t dispatch = window->desktop->dispatch;
  if (!is_callable(dispatch)) {
    fputs("desktop IPC bridge is unavailable\n", stderr);
    return;
  }
  ant_value_t args[] = {
    js_mknum((double)window->identifier),  js_mknum((double)operation),           js_mknum((double)request_id),
    js_mkstr(js, channel, channel_length), js_mkstr(js, payload, payload_length),
  };
  ant_value_t result = ant_desktop_call_function(js, dispatch, js_mkundef(), args, 5);
  if (is_err(result)) { fprintf(stderr, "desktop IPC dispatch failed: %s\n", js_str(js, result)); }
}

void ant_desktop_reject_load(ant_desktop_window_state_t *window, const char *message) {
  if (!window || !window->load_pending) return;
  window->load_pending = false;
  js_reject_promise(window->js, window->load_promise, js_mkerr(window->js, "%s", message));
  window->load_promise = js_mkundef();
}

void ant_desktop_resolve_load(ant_desktop_window_state_t *window) {
  if (!window || !window->load_pending) return;
  window->load_pending = false;
  js_resolve_promise(window->js, window->load_promise, js_mkundef());
  window->load_promise = js_mkundef();
}

bool ant_desktop_has_capability(const ant_desktop_window_state_t *window, const char *access, const char *channel,
                                size_t channel_length) {
  if (!window) return false;
  size_t access_length = strlen(access);
  size_t grant_length = access_length + 1 + channel_length;
  const char *manifest = window->capability_manifest;
  size_t start = 0;
  while (start <= window->capability_manifest_length) {
    const char *separator = memchr(manifest + start, ';', window->capability_manifest_length - start);
    size_t end = separator ? (size_t)(separator - manifest) : window->capability_manifest_length;
    if (end - start == grant_length && memcmp(manifest + start, access, access_length) == 0 &&
        manifest[start + access_length] == ':' &&
        memcmp(manifest + start + access_length + 1, channel, channel_length) == 0)
      return true;
    if (!separator) break;
    start = end + 1;
  }
  return false;
}

void ant_desktop_release_window_object(ant_desktop_window_state_t *window) {
  if (!window) return;
  char key[32];
  ant_desktop_window_key(window->identifier, key);
  js_set(window->js, window->desktop->window_objects, key, js_mkundef());
  ant_desktop_window_destroy(window);
}
