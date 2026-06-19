#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "inspector.h"
#include "../inspector/bind.h"
#include "internal.h"

static bool repl_preview_ident_char(char c, bool first) {
  unsigned char uc = (unsigned char)c;
  return c == '_' || c == '$' || isalpha(uc) || (!first && isdigit(uc));
}

static void repl_preview_copy_line(
  char *out,
  size_t out_len,
  const char *src,
  size_t src_len
) {
  if (!out || out_len == 0) return;
  out[0] = '\0';
  if (!src) return;

  size_t n = 0;
  bool clipped = false;
  bool pending_space = false;
  for (size_t i = 0; i < src_len && src[i]; i++) {
    if (isspace((unsigned char)src[i])) {
      pending_space = n > 0;
      continue;
    }
    if (pending_space) {
      if (n + 1 >= out_len) {
        clipped = true;
        break;
      }
      out[n++] = ' ';
      pending_space = false;
    }
    if (n + 1 >= out_len) {
      clipped = true;
      break;
    }
    out[n++] = src[i];
  }

  if (clipped && out_len > 5) {
    size_t max_n = out_len - 5;
    if (n > max_n) n = max_n;
    memcpy(out + n, " ...", 4);
    n += 4;
  }
  out[n] = '\0';
}

static bool repl_preview_write_suffix(
  char *out,
  size_t out_len,
  const char *suffix,
  size_t suffix_len
) {
  if (!out || out_len == 0 || !suffix || suffix_len == 0) return false;
  if (suffix_len >= out_len) suffix_len = out_len - 1;
  memcpy(out, suffix, suffix_len);
  out[suffix_len] = '\0';
  return out[0] != '\0';
}

static bool repl_preview_format_value(
  ant_t *js,
  ant_value_t value,
  char *out,
  size_t out_len
) {
  if (!js || !out || out_len == 0) return false;
  out[0] = '\0';

  if (vtype(value) == T_STR) {
    char *str = js_getstr(js, value, NULL);
    if (!str) return false;
    if (out_len > 2) {
      out[0] = '\'';
      repl_preview_copy_line(out + 1, out_len - 2, str, strlen(str));
      size_t n = strlen(out);
      if (n + 1 < out_len) {
        out[n++] = '\'';
        out[n] = '\0';
      }
    }
    return out[0] != '\0';
  }

  char cbuf[512];
  js_cstr_t cstr = js_to_cstr(js, value, cbuf, sizeof(cbuf));
  repl_preview_copy_line(out, out_len, cstr.ptr, strlen(cstr.ptr));
  if (cstr.needs_free) free((void *)cstr.ptr);
  return out[0] != '\0';
}

void repl_preview_snapshot_free(repl_preview_snapshot_t *snapshot) {
  if (!snapshot) return;
  for (size_t i = 0; i < snapshot->count; i++) {
    free(snapshot->items[i].expr);
  }
  free(snapshot->items);
  snapshot->items = NULL;
  snapshot->count = 0;
  snapshot->cap = 0;
}

static bool repl_preview_snapshot_contains(
  const repl_preview_snapshot_t *snapshot,
  const char *expr,
  size_t expr_len
) {
  if (!snapshot || !expr) return false;
  for (size_t i = 0; i < snapshot->count; i++)
    if (
      snapshot->items[i].expr_len == expr_len &&
      memcmp(snapshot->items[i].expr, expr, expr_len) == 0
    ) return true;
  return false;
}

static bool repl_preview_expr_is_ident(const char *expr, size_t expr_len) {
  if (!expr || expr_len == 0 || !repl_preview_ident_char(expr[0], true)) return false;
  for (size_t i = 1; i < expr_len; i++)
    if (!repl_preview_ident_char(expr[i], false)) return false;
  return true;
}

static bool repl_preview_snapshot_add(
  repl_preview_snapshot_t *snapshot,
  const char *expr,
  size_t expr_len
) {
  if (!snapshot || !expr || expr_len == 0) return true;
  if (repl_preview_snapshot_contains(snapshot, expr, expr_len)) return true;

  if (snapshot->count >= snapshot->cap) {
    size_t new_cap = snapshot->cap ? snapshot->cap * 2 : 64;
    repl_preview_entry_t *items = realloc(snapshot->items, new_cap * sizeof(*items));
    if (!items) return false;
    snapshot->items = items;
    snapshot->cap = new_cap;
  }

  char *expr_copy = malloc(expr_len + 1);
  if (!expr_copy) return false;
  memcpy(expr_copy, expr, expr_len);
  expr_copy[expr_len] = '\0';

  snapshot->items[snapshot->count++] = (repl_preview_entry_t){
    .expr = expr_copy,
    .expr_len = expr_len,
  };
  return true;
}

bool repl_preview_snapshot_build(
  ant_t *js,
  const repl_decl_registry_t *decls,
  repl_preview_snapshot_t *snapshot
) {
  if (!js || !snapshot) return false;

  ant_value_t global = js_glob(js);
  if (decls) for (size_t i = 0; i < decls->count; i++) {
    const repl_decl_name_t *decl = &decls->items[i];
    if (!decl->name || decl->len == 0) continue;
    if (!repl_preview_snapshot_add(snapshot, decl->name, decl->len)) return false;
  }

  if (!repl_preview_snapshot_add(snapshot, "this", 4)) return false;
  if (!repl_preview_snapshot_add(snapshot, "global", 6)) return false;
  if (!repl_preview_snapshot_add(snapshot, "globalThis", 10)) return false;

  ant_iter_t iter = js_prop_iter_begin(js, global);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = js_mkundef();
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    if (!repl_preview_expr_is_ident(key, key_len)) continue;
    if (!repl_preview_snapshot_add(snapshot, key, key_len)) {
      js_prop_iter_end(&iter);
      return false;
    }
  }
  js_prop_iter_end(&iter);
  return true;
}

static bool repl_preview_snapshot_complete(
  const repl_preview_snapshot_t *snapshot,
  const char *line,
  size_t len,
  char *suffix_out,
  size_t suffix_len
) {
  if (!snapshot || !line || !suffix_out || suffix_len == 0) return false;
  if (repl_preview_snapshot_contains(snapshot, line, len)) return false;

  const repl_preview_entry_t *prefix_match = NULL;
  size_t prefix_match_count = 0;
  size_t common_len = 0;

  for (size_t i = 0; i < snapshot->count; i++) {
    const repl_preview_entry_t *entry = &snapshot->items[i];
    if (len < 3) continue;
    if (entry->expr_len > len && memcmp(entry->expr, line, len) == 0) {
      if (!prefix_match) {
        prefix_match = entry;
        common_len = entry->expr_len;
      } else {
        size_t max_common =
          common_len < entry->expr_len ? common_len : entry->expr_len;
        size_t j = len;
        while (j < max_common && prefix_match->expr[j] == entry->expr[j]) j++;
        common_len = j;
      }
      prefix_match_count++;
    }
  }

  if (prefix_match) {
    size_t completed_len =
      prefix_match_count == 1 ? prefix_match->expr_len : common_len;
    if (completed_len <= len) return false;
    return repl_preview_write_suffix(
      suffix_out,
      suffix_len,
      prefix_match->expr + len,
      completed_len - len
    );
  }

  return false;
}

static bool repl_preview_member_complete(
  ant_t *js,
  const char *line,
  size_t len,
  char *suffix_out,
  size_t suffix_len
) {
  if (!js || !line || !suffix_out || suffix_len == 0) return false;

  size_t dot = len;
  while (dot > 0 && line[dot - 1] != '.') dot--;
  if (dot == 0) return false;
  dot--;

  size_t prefix_start = dot + 1;
  size_t prefix_len = len - prefix_start;
  const char *prefix = line + prefix_start;
  if (prefix_len > 0 && !repl_preview_expr_is_ident(prefix, prefix_len))
    return false;

  ant_value_t obj = js_mkundef();
  if (!inspector_eval_safe_member_expr(js, line, dot, &obj)) return false;

  const char *match = NULL;
  size_t match_len = 0;
  size_t match_count = 0;
  size_t common_len = 0;

  ant_iter_t iter = js_prop_iter_begin(js, obj);
  const char *key = NULL;
  size_t key_len = 0;
  ant_value_t value = js_mkundef();
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    (void)value;
    if (!repl_preview_expr_is_ident(key, key_len)) continue;
    if (key_len < prefix_len || memcmp(key, prefix, prefix_len) != 0)
      continue;
    if (key_len == prefix_len) {
      js_prop_iter_end(&iter);
      return false;
    }

    if (!match) {
      match = key;
      match_len = key_len;
      common_len = key_len;
    } else {
      size_t max_common = common_len < key_len ? common_len : key_len;
      size_t j = prefix_len;
      while (j < max_common && match[j] == key[j]) j++;
      common_len = j;
    }
    match_count++;
  }
  js_prop_iter_end(&iter);

  if (!match) return false;
  size_t completed_len = match_count == 1 ? match_len : common_len;
  if (completed_len <= prefix_len) return false;

  return repl_preview_write_suffix(
    suffix_out,
    suffix_len,
    match + prefix_len,
    completed_len - prefix_len
  );
}

bool repl_preview_compute(
  ant_t *js,
  const repl_preview_snapshot_t *snapshot,
  const char *line,
  size_t len,
  char *suffix_out,
  size_t suffix_len,
  char *preview_out,
  size_t preview_len
) {
  if (
    !js || !line ||
    !suffix_out || suffix_len == 0 ||
    !preview_out || preview_len == 0
  ) return false;
  suffix_out[0] = '\0';
  preview_out[0] = '\0';

  while (len > 0 && isspace((unsigned char)*line)) {
    line++;
    len--;
  }
  while (len > 0 && isspace((unsigned char)line[len - 1])) len--;
  if (len == 0 || line[0] == '.') return false;

  if (memchr(line, '.', len))
    repl_preview_member_complete(js, line, len, suffix_out, suffix_len);
  else
    repl_preview_snapshot_complete(snapshot, line, len, suffix_out, suffix_len);

  char completed[REPL_PREVIEW_EXPR_MAX];
  const char *preview_expr = line;
  size_t preview_expr_len = len;
  if (suffix_out[0] != '\0') {
    size_t suffix_bytes = strlen(suffix_out);
    if (len + suffix_bytes < sizeof(completed)) {
      memcpy(completed, line, len);
      memcpy(completed + len, suffix_out, suffix_bytes);
      completed[len + suffix_bytes] = '\0';
      preview_expr = completed;
      preview_expr_len = len + suffix_bytes;
    }
  }

  ant_value_t value = js_mkundef();
  if (inspector_eval_safe_expr(js, preview_expr, preview_expr_len, &value))
    repl_preview_format_value(js, value, preview_out, preview_len);

  return suffix_out[0] != '\0' || preview_out[0] != '\0';
}
