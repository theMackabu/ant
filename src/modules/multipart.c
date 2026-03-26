#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"

#include "modules/blob.h"
#include "modules/formdata.h"
#include "modules/multipart.h"
#include "modules/url.h"

static bool ct_is_type(const char *ct, const char *type) {
  size_t type_len = 0;

  if (!ct || !type) return false;

  while (*ct == ' ' || *ct == '\t') ct++;
  type_len = strlen(type);
  if (strncasecmp(ct, type, type_len) != 0) return false;

  ct += type_len;
  return *ct == '\0' || *ct == ';' || *ct == ' ' || *ct == '\t';
}

static const char *ct_find_next_param(const char *p) {
scan:
  if (*p == '\0') return NULL;
  if (*p == ';') return p;
  if (*p != '"') {
    p++;
    goto scan;
  }

quoted:
  p++;
  if (*p == '\0') return NULL;
  if (*p == '\\') goto escaped;
  if (*p == '"') {
    p++;
    goto scan;
  }
  goto quoted;

escaped:
  p++;
  if (*p == '\0') return NULL;
  p++;
  goto quoted;
}

static char *ct_get_param_dup(const char *ct, const char *name) {
  const char *p = NULL;
  const char *param_name = NULL;
  const char *param_name_end = NULL;
  
  char *buf = NULL;
  size_t name_len = 0;
  size_t len = 0;
  size_t cap = 0;
  char ch = '\0';

  if (!ct || !name) return NULL;

  name_len = strlen(name);
  if (name_len == 0) return NULL;

  p = strchr(ct, ';');

next_param:
  if (!p) return NULL;
  p++;

  while (*p == ' ' || *p == '\t') p++;
  if (*p == '\0') return NULL;

  param_name = p;
  while (*p && *p != '=' && *p != ';') p++;
  param_name_end = p;

  while (
    param_name_end > param_name &&
    (param_name_end[-1] == ' ' || param_name_end[-1] == '\t')
  ) param_name_end--;

  if (*p != '=') goto skip_param;
  if ((size_t)(param_name_end - param_name) != name_len) goto skip_param;
  if (strncasecmp(param_name, name, name_len) != 0) goto skip_param;

  p++;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '\0') return strdup("");

  if (*p == '"') {
    p++;
    goto quoted;
  }

unquoted:
  if (*p == '\0' || *p == ';' || *p == ' ' || *p == '\t') {
    buf = malloc(len + 1);
    if (!buf) return NULL;
    if (len != 0) memcpy(buf, p - len, len);
    buf[len] = '\0';
    return buf;
  }
  p++;
  len++;
  goto unquoted;

quoted:
  if (*p == '\0') goto fail;
  if (*p == '"') goto done;
  if (*p == '\\') goto quoted_escape;
  ch = *p++;
  goto append;

quoted_escape:
  p++;
  if (*p == '\0') goto fail;
  ch = *p++;
  goto append;

append:
  if (len == cap) {
    size_t next_cap = cap ? cap * 2 : 32;
    char *next = realloc(buf, next_cap);
    if (!next) goto fail;
    buf = next;
    cap = next_cap;
  }
  buf[len++] = ch;
  goto quoted;

done:
  p++;
  if (len == cap) {
    size_t next_cap = cap ? cap + 1 : 1;
    char *next = realloc(buf, next_cap);
    if (!next) goto fail;
    buf = next;
    cap = next_cap;
  }
  buf[len] = '\0';
  return buf;

skip_param:
  p = ct_find_next_param(p);
  goto next_param;

fail:
  free(buf);
  return NULL;
}

static ant_value_t parse_formdata_urlencoded(ant_t *js, const uint8_t *data, size_t size) {
  ant_value_t fd = 0;
  char *body = NULL;
  char *cursor = NULL;

  fd = formdata_create_empty(js);
  if (is_err(fd)) return fd;
  if (!data || size == 0) return fd;

  body = strndup((const char *)data, size);
  if (!body) return js_mkerr(js, "out of memory");

  cursor = body;
  while (cursor) {
    ant_value_t r = 0;
    char *amp = strchr(cursor, '&');
    char *eq = NULL;
    char *raw_name = NULL;
    char *raw_value = NULL;
    char *name = NULL;
    char *value = NULL;

    if (amp) *amp = '\0';

    eq = strchr(cursor, '=');
    raw_name = cursor;
    raw_value = eq ? (eq + 1) : "";
    if (eq) *eq = '\0';

    name = form_urldecode(raw_name);
    value = form_urldecode(raw_value);
    if (!name || !value) {
      free(name);
      free(value);
      free(body);
      return js_mkerr(js, "out of memory");
    }

    r = formdata_append_string(
      js, fd,
      js_mkstr(js, name, strlen(name)),
      js_mkstr(js, value, strlen(value))
    );
    
    free(name);
    free(value);
    
    if (is_err(r)) {
      free(body);
      return r;
    }
    
    cursor = amp ? (amp + 1) : NULL;
  }

  free(body);
  return fd;
}

static const uint8_t *find_bytes(
  const uint8_t *haystack, size_t haystack_len,
  const uint8_t *needle, size_t needle_len
) {
  size_t i = 0;

  if (needle_len == 0) return haystack;
  if (haystack_len < needle_len) return NULL;

  for (i = 0; i + needle_len <= haystack_len; i++) {
    if (memcmp(haystack + i, needle, needle_len) == 0) return haystack + i;
  }
  
  return NULL;
}

static ant_value_t parse_formdata_multipart(
  ant_t *js, const uint8_t *data, size_t size, const char *body_type
) {
  ant_value_t fd = 0;
  char *boundary = NULL;
  char *delim = NULL;
  
  const uint8_t *p = data;
  const uint8_t *end = data + size;
  size_t delim_len = 0;

  if (!data || size == 0) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to parse body as FormData");
  }

  boundary = ct_get_param_dup(body_type, "boundary");
  if (!boundary || boundary[0] == '\0') goto invalid;

  fd = formdata_create_empty(js);
  if (is_err(fd)) goto done;

  delim_len = strlen(boundary) + 2;
  delim = malloc(delim_len + 1);
  if (!delim) {
    fd = js_mkerr(js, "out of memory");
    goto done;
  }
  snprintf(delim, delim_len + 1, "--%s", boundary);

  if ((size_t)(end - p) < delim_len || memcmp(p, delim, delim_len) != 0) goto invalid;
  p += delim_len;

next_part:
  if (p > end) goto done;
  if ((size_t)(end - p) >= 2 && memcmp(p, "--", 2) == 0) goto done;
  if ((size_t)(end - p) < 2 || memcmp(p, "\r\n", 2) != 0) goto invalid;
  p += 2;

  {
    ant_value_t r = 0;
    const uint8_t *hdr_end = find_bytes(
      p, (size_t)(end - p), (const uint8_t *)"\r\n\r\n", 4
    );
    
    char *headers = NULL;
    char *name = NULL;
    char *filename = NULL;
    char *part_type = NULL;
    char *marker = NULL;
    const uint8_t *part_end = NULL;

    if (!hdr_end) goto invalid;

    headers = strndup((const char *)p, (size_t)(hdr_end - p));
    if (!headers) {
      fd = js_mkerr(js, "out of memory");
      goto done;
    }
    p = hdr_end + 4;

    {
      char *saveptr = NULL;
      for (char *line = strtok_r(headers, "\r\n", &saveptr);
           line;
           line = strtok_r(NULL, "\r\n", &saveptr)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';

        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;

        if (strcasecmp(line, "Content-Disposition") == 0) {
          char *name_pos = strcasestr(value, "name=");
          char *file_pos = strcasestr(value, "filename=");

          if (name_pos) {
            name_pos += 5;
            if (*name_pos == '"') {
              char *endq = NULL;
              name_pos++;
              endq = strchr(name_pos, '"');
              if (endq) name = strndup(name_pos, (size_t)(endq - name_pos));
            }
          }

          if (file_pos) {
            file_pos += 9;
            if (*file_pos == '"') {
              char *endq = NULL;
              file_pos++;
              endq = strchr(file_pos, '"');
              if (endq) filename = strndup(file_pos, (size_t)(endq - file_pos));
            } else {
              char *ende = file_pos;
              while (*ende && *ende != ';') ende++;
              filename = strndup(file_pos, (size_t)(ende - file_pos));
            }
          }
        } else if (strcasecmp(line, "Content-Type") == 0) {
          part_type = strdup(value);
        }
      }
    }
    free(headers);
    headers = NULL;

    if (!name) {
      free(filename);
      free(part_type);
      goto invalid;
    }

    marker = malloc(delim_len + 3);
    if (!marker) {
      free(name);
      free(filename);
      free(part_type);
      fd = js_mkerr(js, "out of memory");
      goto done;
    }
    snprintf(marker, delim_len + 3, "\r\n%s", delim);
    part_end = find_bytes(
      p, (size_t)(end - p), (const uint8_t *)marker, delim_len + 2
    );
    free(marker);
    marker = NULL;

    if (!part_end) {
      free(name);
      free(filename);
      free(part_type);
      goto invalid;
    }

    if (filename) {
      ant_value_t blob = blob_create(
        js, p, (size_t)(part_end - p), part_type ? part_type : ""
      );
      r = is_err(blob)
        ? blob
        : formdata_append_file(
            js, fd,
            js_mkstr(js, name, strlen(name)),
            blob,
            js_mkstr(js, filename, strlen(filename))
          );
    } else {
      r = formdata_append_string(
        js, fd,
        js_mkstr(js, name, strlen(name)),
        js_mkstr(js, p, (size_t)(part_end - p))
      );
    }

    free(name);
    free(filename);
    free(part_type);
    if (is_err(r)) {
      fd = r;
      goto done;
    }

    p = part_end + 2 + delim_len;
    if ((size_t)(end - p) >= 2 && memcmp(p, "--", 2) == 0) goto done;
  }

  goto next_part;

invalid:
  fd = js_mkerr_typed(js, JS_ERR_TYPE, "Failed to parse body as FormData");

done:
  free(boundary);
  free(delim);
  return fd;
}

ant_value_t formdata_parse_body(
  ant_t *js, const uint8_t *data, size_t size,
  const char *body_type, bool has_body
) {
  if (body_type && ct_is_type(body_type, "application/x-www-form-urlencoded")) {
    return parse_formdata_urlencoded(js, data, size);
  }

  if (body_type && ct_is_type(body_type, "multipart/form-data")) {
    if (!has_body || !data || size == 0) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to parse body as FormData");
    }
    return parse_formdata_multipart(js, data, size, body_type);
  }

  return js_mkerr_typed(js, JS_ERR_TYPE, "Failed to parse body as FormData");
}

typedef struct {
  uint8_t *buf;
  size_t size;
  size_t cap;
} mp_buf_t;

static bool mp_grow(mp_buf_t *b, size_t need) {
  size_t nc = 0;
  uint8_t *nb = NULL;

  if (b->size + need <= b->cap) return true;

  nc = b->cap ? b->cap * 2 : 4096;
  while (nc < b->size + need) nc *= 2;

  nb = realloc(b->buf, nc);
  if (!nb) return false;
  b->buf = nb;
  b->cap = nc;
  return true;
}

static bool mp_append(mp_buf_t *b, const void *data, size_t len) {
  if (!mp_grow(b, len)) return false;
  memcpy(b->buf + b->size, data, len);
  b->size += len;
  return true;
}

static bool mp_append_str(mp_buf_t *b, const char *s) {
  return mp_append(b, s, strlen(s));
}

uint8_t *formdata_serialize_multipart(
  ant_t *js, ant_value_t fd, size_t *out_size, char **out_boundary
) {
  ant_value_t values_arr = js_get_slot(fd, SLOT_ENTRIES);
  ant_value_t data_slot = js_get_slot(fd, SLOT_DATA);
  char boundary[49];
  mp_buf_t b = {NULL, 0, 0};
  fd_data_t *d = NULL;
  if (vtype(data_slot) != T_NUM) return NULL;
  d = (fd_data_t *)(uintptr_t)(size_t)js_getnum(data_slot);
  if (!d) return NULL;

  snprintf(
    boundary, sizeof(boundary),
    "----AntFormBoundary%08x%08x", (unsigned)rand(), (unsigned)rand()
  );

  if (d->count == 0) {
    uint8_t *empty = malloc(1);
    if (!empty) return NULL;
    *out_size = 0;
    *out_boundary = strdup(boundary);
    if (!*out_boundary) {
      free(empty);
      return NULL;
    }
    return empty;
  }

  for (fd_entry_t *e = d->head; e; e = e->next) {
    if (!mp_append_str(&b, "--") ||
        !mp_append_str(&b, boundary) ||
        !mp_append_str(&b, "\r\n")) {
      goto oom;
    }

    if (!e->is_file) {
      char disp[512];
      const char *val = e->str_value ? e->str_value : "";

      snprintf(
        disp, sizeof(disp),
        "Content-Disposition: form-data; name=\"%s\"\r\n\r\n",
        e->name ? e->name : ""
      );
      if (!mp_append_str(&b, disp) || !mp_append_str(&b, val)) goto oom;
    } else {
      ant_value_t file_val = js_arr_get(js, values_arr, (ant_offset_t)e->val_idx);
      blob_data_t *bd = blob_get_data(file_val);
      const char *filename = (bd && bd->name) ? bd->name : "blob";
      const char *mime = (bd && bd->type && bd->type[0])
        ? bd->type
        : "application/octet-stream";
      char disp[1024];

      snprintf(
        disp, sizeof(disp),
        "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        e->name ? e->name : "", filename, mime
      );
      if (!mp_append_str(&b, disp)) goto oom;
      if (bd && bd->data && bd->size > 0 && !mp_append(&b, bd->data, bd->size)) goto oom;
    }

    if (!mp_append_str(&b, "\r\n")) goto oom;
  }

  if (!mp_append_str(&b, "--") ||
      !mp_append_str(&b, boundary) ||
      !mp_append_str(&b, "--\r\n")) {
    goto oom;
  }

  *out_size = b.size;
  *out_boundary = strdup(boundary);
  if (!*out_boundary) goto oom;
  return b.buf;

oom:
  free(b.buf);
  return NULL;
}
