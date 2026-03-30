#include <compat.h> // IWYU pragma: keep

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "modules/symbol.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifdef _WIN32
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"
#define PATH_DELIMITER ';'
#else
#define PATH_SEP '/'
#define PATH_SEP_STR "/"
#define PATH_DELIMITER ':'
#endif

typedef enum {
  PATH_STYLE_POSIX = 1,
  PATH_STYLE_WIN32 = 2,
} path_style_t;

static path_style_t path_host_style(void) {
#ifdef _WIN32
  return PATH_STYLE_WIN32;
#else
  return PATH_STYLE_POSIX;
#endif
}

static path_style_t path_current_style(ant_t *js) {
  ant_value_t data = js_get_slot(js->current_func, SLOT_DATA);
  if (vtype(data) == T_NUM) {
    int style = (int)js_getnum(data);
    if (style == PATH_STYLE_WIN32) return PATH_STYLE_WIN32;
    if (style == PATH_STYLE_POSIX) return PATH_STYLE_POSIX;
  }
  return path_host_style();
}

static bool path_style_is_windows(path_style_t style) {
  return style == PATH_STYLE_WIN32;
}

static bool path_is_sep(path_style_t style, char ch) {
  if (style == PATH_STYLE_WIN32) return ch == '\\' || ch == '/';
  return ch == '/';
}

static char path_sep_char(path_style_t style) {
  return style == PATH_STYLE_WIN32 ? '\\' : '/';
}

static const char *path_sep_str(path_style_t style) {
  return style == PATH_STYLE_WIN32 ? "\\" : "/";
}

static const char *path_delimiter_str(path_style_t style) {
  return style == PATH_STYLE_WIN32 ? ";" : ":";
}

static bool path_has_drive_letter(const char *path, size_t len) {
  return len >= 2 && isalpha((unsigned char)path[0]) && path[1] == ':';
}

static bool path_is_absolute_style(path_style_t style, const char *path, size_t len) {
  if (!path || len == 0) return false;
  if (style == PATH_STYLE_WIN32) {
    if (path_is_sep(style, path[0])) return true;
    return len >= 3 && path_has_drive_letter(path, len) && path_is_sep(style, path[2]);
  }
  return path[0] == '/';
}

static char *path_normalize_separators(path_style_t style, const char *path, size_t len) {
  char *result = malloc(len + 1);
  if (!result) return NULL;

  for (size_t i = 0; i < len; i++) {
    if (style == PATH_STYLE_WIN32 && (path[i] == '\\' || path[i] == '/'))
      result[i] = '\\';
    else result[i] = path[i];
  }

  result[len] = '\0';
  return result;
}

static size_t path_root_length(path_style_t style, const char *path, size_t len) {
  if (!path || len == 0) return 0;
  if (style != PATH_STYLE_WIN32) return path[0] == '/' ? 1 : 0;

  if (len >= 2 && path_is_sep(style, path[0]) && path_is_sep(style, path[1])) {
    size_t i = 2;
    int parts = 0;

    while (i < len) {
    while (i < len && path_is_sep(style, path[i])) i++;
    
    size_t start = i;
    while (i < len && !path_is_sep(style, path[i])) i++;
    
    if (i == start) continue;
    parts++;
    
    if (parts == 2) {
      while (i < len && path_is_sep(style, path[i])) i++;
      return i;
    }}
    
    return 2;
  }

  if (path_has_drive_letter(path, len))
    return (len >= 3 && path_is_sep(style, path[2])) ? 3 : 2;

  return path_is_sep(style, path[0]) ? 1 : 0;
}

static bool path_is_drive_relative(path_style_t style, const char *path, size_t len) {
  return style == PATH_STYLE_WIN32 && path_has_drive_letter(path, len) && !(len >= 3 && path_is_sep(style, path[2]));
}

typedef struct {
  char *from_norm;
  char *to_norm;
  char **from_segs;
  char **to_segs;
  size_t *from_lens;
  size_t *to_lens;
  int from_count;
  int to_count;
  size_t from_root_len;
  size_t to_root_len;
  int common;
  char *result;
  size_t result_cap;
  size_t pos;
} path_relative_ctx_t;

static size_t path_trimmed_end(path_style_t style, const char *path, size_t len) {
  size_t root_len = path_root_length(style, path, len);

  while (len > root_len && path_is_sep(style, path[len - 1])) len--;
  return len;
}

static size_t path_basename_start(path_style_t style, const char *path, size_t len) {
  size_t root_len = path_root_length(style, path, len);
  size_t end = path_trimmed_end(style, path, len);
  size_t start = end;

  while (start > root_len && !path_is_sep(style, path[start - 1])) start--;
  return start;
}

static ant_value_t path_make_string(ant_t *js, const char *src, size_t len) {
  return js_mkstr(js, src, len);
}

static ant_value_t builtin_path_basename(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  if (nargs < 1) return js_mkerr(js, "basename() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "basename() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkstr(js, "", 0);

  size_t end = path_trimmed_end(style, path, path_len);
  size_t start = path_basename_start(style, path, path_len);
  const char *base = path + start;
  size_t base_len = end > start ? end - start : 0;
  
  if (nargs >= 2 && vtype(args[1]) == T_STR) {
    size_t ext_len;
    char *ext = js_getstr(js, args[1], &ext_len);
    
    if (ext && ext_len > 0 && base_len >= ext_len) {
      if (memcmp(base + base_len - ext_len, ext, ext_len) == 0) base_len -= ext_len;
    }
  }
  
  return path_make_string(js, base, base_len);
}

// path.dirname(path)
static ant_value_t builtin_path_dirname(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  
  if (nargs < 1) return js_mkerr(js, "dirname() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "dirname() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  
  if (!path || path_len == 0) return js_mkstr(js, ".", 1);
  char *normalized = path_normalize_separators(style, path, path_len);
  
  size_t root_len = 0;
  size_t end = 0;
  size_t cut = 0;
  
  if (!normalized) return js_mkerr(js, "Out of memory");
  root_len = path_root_length(style, normalized, path_len);
  end = path_trimmed_end(style, normalized, path_len);
  cut = end;

  while (cut > root_len && !path_is_sep(style, normalized[cut - 1])) cut--;
  while (cut > root_len && path_is_sep(style, normalized[cut - 1])) cut--;

  if (cut == 0) {
    free(normalized);
    return js_mkstr(js, ".", 1);
  }

  if (cut < root_len) cut = root_len;

  ant_value_t result = path_make_string(js, normalized, cut);
  free(normalized);
  return result;
}

// path.extname(path)
static ant_value_t builtin_path_extname(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  
  if (nargs < 1) return js_mkerr(js, "extname() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "extname() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkstr(js, "", 0);

  size_t start = path_basename_start(style, path, path_len);
  size_t end = path_trimmed_end(style, path, path_len);
  const char *base = path + start;
  size_t base_len = end > start ? end - start : 0;

  for (size_t i = base_len; i > 0; i--) {
    if (base[i - 1] != '.') continue;
    if (i - 1 == 0) break;
    return path_make_string(js, base + i - 1, base_len - (i - 1));
  }

  return js_mkstr(js, "", 0);
}

static int is_dotdot(const char *seg, size_t len) {
  return len == 2 && seg[0] == '.' && seg[1] == '.';
}

static char* normalize_path_full(path_style_t style, const char *path, size_t path_len) {
  char *normalized = NULL;
  char **segments = NULL;
  size_t *seg_lens = NULL;
  
  char *result = NULL;
  char sep = path_sep_char(style);
  
  size_t root_len = 0;
  bool drive_relative = false;
  
  if (path_len == 0) return strdup(".");
  
  normalized = path_normalize_separators(style, path, path_len);
  if (!normalized) goto fail;
  
  segments = malloc(path_len * sizeof(char*));
  seg_lens = malloc(path_len * sizeof(size_t));
  if (!segments || !seg_lens) goto fail;
  
  root_len = path_root_length(style, normalized, path_len);
  drive_relative = path_is_drive_relative(style, normalized, path_len);
  int seg_count = 0;
  
  char *start = normalized + root_len;
  char *seg_start = start;
  
  for (char *p = start; ; p++) {
    if (!path_is_sep(style, *p) && *p != '\0') continue;
    
    size_t len = p - seg_start;
    if (len == 0 || (len == 1 && seg_start[0] == '.')) goto next;
    
    if (is_dotdot(seg_start, len)) {
      if (seg_count > 0 && !is_dotdot(segments[seg_count - 1], seg_lens[seg_count - 1])) seg_count--;
      else if (root_len == 0 || drive_relative) goto add_segment;
      goto next;
    }
    
  add_segment:
    segments[seg_count] = seg_start;
    seg_lens[seg_count] = len;
    seg_count++;
    
  next:
    if (*p == '\0') break;
    seg_start = p + 1;
  }
  
  size_t result_len = root_len;
  for (int i = 0; i < seg_count; i++) {
    result_len += seg_lens[i];
    if (i < seg_count - 1) result_len++;
  }
  if (result_len == 0) result_len = 1;
  
  result = malloc(result_len + 1);
  if (!result) goto fail;
  
  size_t pos = 0;
  if (root_len > 0) {
    memcpy(result + pos, normalized, root_len);
    pos += root_len;
  }
  
  for (int i = 0; i < seg_count; i++) {
    if (pos > 0 && result[pos - 1] != sep && !(drive_relative && pos == root_len))
      result[pos++] = sep;
    memcpy(result + pos, segments[i], seg_lens[i]);
    pos += seg_lens[i];
  }
  
  if (pos == 0) result[pos++] = '.';
  result[pos] = '\0';
  
  free(segments);
  free(seg_lens);
  free(normalized);
  return result;

fail:
  free(segments);
  free(seg_lens);
  free(normalized);
  return NULL;
}

// path.normalize(path)
static ant_value_t builtin_path_normalize(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  if (nargs < 1) return js_mkerr(js, "normalize() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "normalize() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkstr(js, ".", 1);
  
  char *result = normalize_path_full(style, path, path_len);
  if (!result) return js_mkerr(js, "Out of memory");
  
  ant_value_t ret = js_mkstr(js, result, strlen(result));
  free(result);
  return ret;
}

// path.join(...paths)
static ant_value_t builtin_path_join(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  char sep = path_sep_char(style);
  if (nargs < 1) return js_mkstr(js, ".", 1);
  
  size_t total_len = 0;
  char **segments = malloc(nargs * sizeof(char*));
  size_t *lengths = malloc(nargs * sizeof(size_t));
  
  if (!segments || !lengths) {
    free(segments);
    free(lengths);
    return js_mkerr(js, "Out of memory");
  }
  
  int valid_segments = 0;
  
  for (int i = 0; i < nargs; i++) {
  if (vtype(args[i]) == T_STR) {
  segments[valid_segments] = js_getstr(js, args[i], &lengths[valid_segments]);
  if (segments[valid_segments] && lengths[valid_segments] > 0) {
    total_len += lengths[valid_segments] + 1; // +1 for separator
    valid_segments++;
  }}}
  
  if (valid_segments == 0) {
    free(segments);
    free(lengths);
    return js_mkstr(js, ".", 1);
  }
  
  char *result = malloc(total_len + 1);
  if (!result) {
    free(segments);
    free(lengths);
    return js_mkerr(js, "Out of memory");
  }
  
  size_t pos = 0;
  for (int i = 0; i < valid_segments; i++) {
    if (i > 0 && pos > 0 && result[pos - 1] != sep) {
      result[pos++] = sep;
    }
    
    size_t start = 0;
    if (i > 0 && path_is_sep(style, segments[i][0])) start = 1;
    
    size_t seg_len = lengths[i];
    while (seg_len > start && path_is_sep(style, segments[i][seg_len - 1])) seg_len--;
    
    if (seg_len > start) {
      memcpy(result + pos, segments[i] + start, seg_len - start);
      pos += seg_len - start;
    }
  }
  
  result[pos] = '\0';
  
  char *normalized = normalize_path_full(style, result, pos);
  free(result);
  free(segments);
  free(lengths);
  
  if (!normalized) return js_mkerr(js, "Out of memory");
  
  ant_value_t ret = js_mkstr(js, normalized, strlen(normalized));
  free(normalized);
  return ret;
}

// path.resolve(...paths)
static ant_value_t builtin_path_resolve(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  
  char cwd[PATH_MAX];
  char *cwd_norm = NULL;
  char *joined = NULL;
  char *normalized = NULL;
  char drive_prefix[3] = {0};
  
  bool saw_absolute = false;
  char sep = path_sep_char(style);

  if (getcwd(cwd, sizeof(cwd)) == NULL)
    return js_mkerr(js, "Failed to get current working directory");

  cwd_norm = path_normalize_separators(style, cwd, strlen(cwd));
  if (!cwd_norm) return js_mkerr(js, "Out of memory");

  for (int i = nargs - 1; i >= 0; i--) {
    char *next = NULL;
    size_t seg_len = 0;
    size_t joined_len = 0;
    size_t next_len = 0;
    char *segment = NULL;
    size_t prefix_len = 0;

    if (vtype(args[i]) != T_STR) continue;

    segment = js_getstr(js, args[i], &seg_len);
    if (!segment || seg_len == 0) continue;

    if (style == PATH_STYLE_WIN32 && path_is_drive_relative(style, segment, seg_len)) {
      if (drive_prefix[0] == '\0') {
        drive_prefix[0] = segment[0];
        drive_prefix[1] = ':';
        drive_prefix[2] = '\0';
      }
      prefix_len = 2;
    }

    if (!joined) {
      joined = strndup(segment + prefix_len, seg_len - prefix_len);
      if (!joined) {
        free(cwd_norm);
        return js_mkerr(js, "Out of memory");
      }
    } else {
      joined_len = strlen(joined);
      next_len = (seg_len - prefix_len) + 1 + joined_len + 1;
      next = malloc(next_len);
      if (!next) {
        free(cwd_norm);
        free(joined);
        return js_mkerr(js, "Out of memory");
      }

      snprintf(next, next_len, "%.*s%c%s", (int)(seg_len - prefix_len), segment + prefix_len, sep, joined);
      free(joined);
      joined = next;
    }

    if (path_is_absolute_style(style, segment, seg_len)) {
      saw_absolute = true;
      break;
    }
  }

  if (!joined) {
    if (style == PATH_STYLE_WIN32 && drive_prefix[0] != '\0') {
      size_t cwd_len = strlen(cwd_norm);
      joined = malloc(2 + cwd_len + 1);
      if (!joined) {
        free(cwd_norm);
        return js_mkerr(js, "Out of memory");
      }
      snprintf(joined, 2 + cwd_len + 1, "%s%s", drive_prefix, cwd_norm);
    } else {
      joined = strdup(cwd_norm);
      if (!joined) {
        free(cwd_norm);
        return js_mkerr(js, "Out of memory");
      }
    }
  } else if (!saw_absolute) {
    size_t cwd_len = strlen(cwd_norm);
    size_t joined_len = strlen(joined);
    size_t prefix_len = (style == PATH_STYLE_WIN32 && drive_prefix[0] != '\0') ? 2 : 0;
    char *next = malloc(prefix_len + cwd_len + 1 + joined_len + 1);
    if (!next) {
      free(cwd_norm);
      free(joined);
      return js_mkerr(js, "Out of memory");
    }

    if (prefix_len > 0)
      snprintf(next, prefix_len + cwd_len + 1 + joined_len + 1, "%s%s%c%s", drive_prefix, cwd_norm, sep, joined);
    else snprintf(next, prefix_len + cwd_len + 1 + joined_len + 1, "%s%c%s", cwd_norm, sep, joined);
    free(joined);
    joined = next;
  }

  normalized = normalize_path_full(style, joined, strlen(joined));
  free(cwd_norm);
  free(joined);
  if (!normalized) return js_mkerr(js, "Out of memory");

  ant_value_t ret = js_mkstr(js, normalized, strlen(normalized));
  free(normalized);
  return ret;
}

// path.relative(from, to)
static ant_value_t builtin_path_relative(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  path_relative_ctx_t rel = {0};
  
  char sep = path_sep_char(style);
  if (nargs < 2) return js_mkerr(js, "relative() requires from and to arguments");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "relative() from must be a string");
  if (vtype(args[1]) != T_STR) return js_mkerr(js, "relative() to must be a string");
  
  size_t from_len, to_len;
  char *from = js_getstr(js, args[0], &from_len);
  char *to = js_getstr(js, args[1], &to_len);
  
  if (!from || !to) return js_mkerr(js, "Failed to get arguments");
  if (from_len == to_len && strncmp(from, to, from_len) == 0) return js_mkstr(js, "", 0);

  rel.from_norm = normalize_path_full(style, from, from_len);
  rel.to_norm = normalize_path_full(style, to, to_len);
  if (!rel.from_norm || !rel.to_norm) goto relative_fail;

  rel.from_root_len = path_root_length(style, rel.from_norm, strlen(rel.from_norm));
  rel.to_root_len = path_root_length(style, rel.to_norm, strlen(rel.to_norm));

  if (style == PATH_STYLE_WIN32) {
    if (rel.from_root_len != rel.to_root_len || strncasecmp(rel.from_norm, rel.to_norm, rel.from_root_len) != 0) {
      ant_value_t out = js_mkstr(js, rel.to_norm, strlen(rel.to_norm));
      free(rel.from_norm);
      free(rel.to_norm);
      return out;
    }
  } else if (rel.from_root_len != rel.to_root_len || strncmp(rel.from_norm, rel.to_norm, rel.from_root_len) != 0) {
    ant_value_t out = js_mkstr(js, rel.to_norm, strlen(rel.to_norm));
    free(rel.from_norm);
    free(rel.to_norm);
    return out;
  }

  rel.from_segs = malloc(strlen(rel.from_norm) * sizeof(char *));
  rel.to_segs = malloc(strlen(rel.to_norm) * sizeof(char *));
  rel.from_lens = malloc(strlen(rel.from_norm) * sizeof(size_t));
  rel.to_lens = malloc(strlen(rel.to_norm) * sizeof(size_t));
  if (!rel.from_segs || !rel.to_segs || !rel.from_lens || !rel.to_lens) goto relative_fail;

  for (size_t i = rel.from_root_len, start = rel.from_root_len;; i++) {
    if (rel.from_norm[i] != '\0' && !path_is_sep(style, rel.from_norm[i])) continue;
    if (i > start) {
      rel.from_segs[rel.from_count] = rel.from_norm + start;
      rel.from_lens[rel.from_count++] = i - start;
    }
    if (rel.from_norm[i] == '\0') break;
    start = i + 1;
  }

  for (size_t i = rel.to_root_len, start = rel.to_root_len;; i++) {
    if (rel.to_norm[i] != '\0' && !path_is_sep(style, rel.to_norm[i])) continue;
    if (i > start) {
      rel.to_segs[rel.to_count] = rel.to_norm + start;
      rel.to_lens[rel.to_count++] = i - start;
    }
    if (rel.to_norm[i] == '\0') break;
    start = i + 1;
  }

  while (rel.common < rel.from_count && rel.common < rel.to_count) {
    bool equal = false;
    if (rel.from_lens[rel.common] == rel.to_lens[rel.common]) {
      equal = style == PATH_STYLE_WIN32
        ? strncasecmp(rel.from_segs[rel.common], rel.to_segs[rel.common], rel.from_lens[rel.common]) == 0
        : strncmp(rel.from_segs[rel.common], rel.to_segs[rel.common], rel.from_lens[rel.common]) == 0;
    }
    if (!equal) break;
    rel.common++;
  }

  rel.result_cap = strlen(rel.to_norm) + (size_t)(rel.from_count - rel.common) * 3 + 2;
  rel.result = malloc(rel.result_cap);
  if (!rel.result) goto relative_fail;

  for (int i = rel.common; i < rel.from_count; i++) {
    if (rel.pos > 0) rel.result[rel.pos++] = sep;
    rel.result[rel.pos++] = '.';
    rel.result[rel.pos++] = '.';
  }

  for (int i = rel.common; i < rel.to_count; i++) {
    if (rel.pos > 0) rel.result[rel.pos++] = sep;
    memcpy(rel.result + rel.pos, rel.to_segs[i], rel.to_lens[i]);
    rel.pos += rel.to_lens[i];
  }

  if (rel.pos == 0) rel.result[rel.pos++] = '.';
  rel.result[rel.pos] = '\0';

  ant_value_t out = js_mkstr(js, rel.result, rel.pos);
  free(rel.result);
  free(rel.from_norm);
  free(rel.to_norm);
  free(rel.from_segs);
  free(rel.to_segs);
  free(rel.from_lens);
  free(rel.to_lens);
  return out;

relative_fail:
  free(rel.result);
  free(rel.from_norm);
  free(rel.to_norm);
  free(rel.from_segs);
  free(rel.to_segs);
  free(rel.from_lens);
  free(rel.to_lens);
  return js_mkerr(js, "Out of memory");
}

// path.isAbsolute(path)
static ant_value_t builtin_path_isAbsolute(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  if (nargs < 1) return js_mkerr(js, "isAbsolute() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "isAbsolute() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_false;

  return js_bool(path_is_absolute_style(style, path, path_len));
}

// path.parse(path)
static ant_value_t builtin_path_parse(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  if (nargs < 1) return js_mkerr(js, "parse() requires a path argument");
  if (vtype(args[0]) != T_STR) return js_mkerr(js, "parse() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  ant_value_t result = js_mkobj(js);
  char *normalized = path_normalize_separators(style, path, path_len);
  size_t root_len = 0;
  size_t start = 0;
  size_t end = 0;
  const char *base = NULL;
  size_t base_len = 0;
  if (!normalized) return js_mkerr(js, "Out of memory");

  root_len = path_root_length(style, normalized, path_len);
  js_set(js, result, "root", path_make_string(js, normalized, root_len));

  end = path_trimmed_end(style, normalized, path_len);
  start = path_basename_start(style, normalized, path_len);
  base = normalized + start;
  base_len = end > start ? end - start : 0;

  if (start == 0) js_set(js, result, "dir", js_mkstr(js, ".", 1));
  else {
    size_t dir_len = start;
    while (dir_len > root_len && path_is_sep(style, normalized[dir_len - 1])) dir_len--;
    js_set(js, result, "dir", path_make_string(js, normalized, dir_len));
  }

  js_set(js, result, "base", path_make_string(js, base, base_len));

  for (size_t i = base_len; i > 0; i--) {
    if (base[i - 1] != '.') continue;
    if (i - 1 == 0) break;
    js_set(js, result, "ext", path_make_string(js, base + i - 1, base_len - (i - 1)));
    js_set(js, result, "name", path_make_string(js, base, i - 1));
    free(normalized);
    return result;
  }

  js_set(js, result, "ext", js_mkstr(js, "", 0));
  js_set(js, result, "name", path_make_string(js, base, base_len));
  free(normalized);
  return result;
}

// path.format(pathObject)
static ant_value_t builtin_path_format(ant_t *js, ant_value_t *args, int nargs) {
  path_style_t style = path_current_style(js);
  
  char sep = path_sep_char(style);
  if (nargs < 1) return js_mkerr(js, "format() requires a path object argument");
  if (!is_special_object(args[0])) return js_mkerr(js, "format() argument must be an object");
  
  ant_value_t obj = args[0];
  
  ant_value_t dir_val = js_get(js, obj, "dir");
  ant_value_t root_val = js_get(js, obj, "root");
  ant_value_t base_val = js_get(js, obj, "base");
  ant_value_t name_val = js_get(js, obj, "name");
  ant_value_t ext_val = js_get(js, obj, "ext");
  
  char result[PATH_MAX] = {0};
  size_t pos = 0;
  
  if (vtype(dir_val) == T_STR) {
    size_t len;
    char *str = js_getstr(js, dir_val, &len);
    if (str && len > 0 && pos + len < PATH_MAX) {
      memcpy(result + pos, str, len);
      pos += len;
      if (result[pos - 1] != sep && pos < PATH_MAX - 1) {
        result[pos++] = sep;
      }
    }
  } else if (vtype(root_val) == T_STR) {
    size_t len;
    char *str = js_getstr(js, root_val, &len);
    if (str && len > 0 && pos + len < PATH_MAX) {
      memcpy(result + pos, str, len);
      pos += len;
    }
  }
  
  if (vtype(base_val) == T_STR) {
    size_t len;
    char *str = js_getstr(js, base_val, &len);
    if (str && len > 0 && pos + len < PATH_MAX) {
      memcpy(result + pos, str, len);
      pos += len;
    }
  } else {
    if (vtype(name_val) == T_STR) {
      size_t len;
      char *str = js_getstr(js, name_val, &len);
      if (str && len > 0 && pos + len < PATH_MAX) {
        memcpy(result + pos, str, len);
        pos += len;
      }
    }
    if (vtype(ext_val) == T_STR) {
      size_t len;
      char *str = js_getstr(js, ext_val, &len);
      if (str && len > 0 && pos + len < PATH_MAX) {
        memcpy(result + pos, str, len);
        pos += len;
      }
    }
  }
  
  return js_mkstr(js, result, pos);
}

typedef struct {
  ant_value_t basename;
  ant_value_t dirname;
  ant_value_t extname;
  ant_value_t join;
  ant_value_t normalize;
  ant_value_t resolve;
  ant_value_t relative;
  ant_value_t isAbsolute;
  ant_value_t parse;
  ant_value_t format;
} path_api_t;

static path_api_t path_build_api_for_style(ant_t *js, path_style_t style) {
ant_value_t style_value = js_mknum((double)style);
return (path_api_t){
  .basename = js_heavy_mkfun(js, builtin_path_basename, style_value),
  .dirname = js_heavy_mkfun(js, builtin_path_dirname, style_value),
  .extname = js_heavy_mkfun(js, builtin_path_extname, style_value),
  .join = js_heavy_mkfun(js, builtin_path_join, style_value),
  .normalize = js_heavy_mkfun(js, builtin_path_normalize, style_value),
  .resolve = js_heavy_mkfun(js, builtin_path_resolve, style_value),
  .relative = js_heavy_mkfun(js, builtin_path_relative, style_value),
  .isAbsolute = js_heavy_mkfun(js, builtin_path_isAbsolute, style_value),
  .parse = js_heavy_mkfun(js, builtin_path_parse, style_value),
  .format = js_heavy_mkfun(js, builtin_path_format, style_value)
};}

static void path_apply_api(ant_t *js, ant_value_t target, const path_api_t *api) {
  js_set(js, target, "basename", api->basename);
  js_set(js, target, "dirname", api->dirname);
  js_set(js, target, "extname", api->extname);
  js_set(js, target, "join", api->join);
  js_set(js, target, "normalize", api->normalize);
  js_set(js, target, "resolve", api->resolve);
  js_set(js, target, "relative", api->relative);
  js_set(js, target, "isAbsolute", api->isAbsolute);
  js_set(js, target, "parse", api->parse);
  js_set(js, target, "format", api->format);
}

static ant_value_t path_make_variant(ant_t *js, const path_api_t *api, path_style_t style) {
  ant_value_t variant = js_mkobj(js);
  path_apply_api(js, variant, api);
  
  js_set(js, variant, "sep", js_mkstr(js, path_sep_str(style), 1));
  js_set(js, variant, "delimiter", js_mkstr(js, path_delimiter_str(style), 1));
  
  return variant;
}

ant_value_t path_library(ant_t *js) {
  path_api_t api = path_build_api_for_style(js, path_host_style());
  path_api_t posix_api = path_build_api_for_style(js, PATH_STYLE_POSIX);
  path_api_t win32_api = path_build_api_for_style(js, PATH_STYLE_WIN32);
  
  ant_value_t lib = path_make_variant(js, &api, path_host_style());
  ant_value_t posix = path_make_variant(js, &posix_api, PATH_STYLE_POSIX);
  ant_value_t win32 = path_make_variant(js, &win32_api, PATH_STYLE_WIN32);

  js_set(js, lib, "posix", posix);
  js_set(js, lib, "win32", win32);
  js_set_sym(js, lib, get_toStringTag_sym(), js_mkstr(js, "path", 4));
  
  return lib;
}
