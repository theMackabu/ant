#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>

#include "ant.h"

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

static char* normalize_separators(const char *path, size_t len) {
  char *result = malloc(len + 1);
  if (!result) return NULL;
  
  for (size_t i = 0; i < len; i++) {
    if (path[i] == '\\' || path[i] == '/') {
      result[i] = PATH_SEP;
    } else {
      result[i] = path[i];
    }
  }
  result[len] = '\0';
  return result;
}

static jsval_t builtin_path_basename(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "basename() requires a path argument");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "basename() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkstr(js, "", 0);
  
  char *path_copy = strndup(path, path_len);
  if (!path_copy) return js_mkerr(js, "Out of memory");
  char *base = basename(path_copy);
  
  if (nargs >= 2 && js_type(args[1]) == JS_STR) {
    size_t ext_len;
    char *ext = js_getstr(js, args[1], &ext_len);
    
    if (ext && ext_len > 0) {
      size_t base_len = strlen(base);
      if (base_len >= ext_len && strcmp(base + base_len - ext_len, ext) == 0) base[base_len - ext_len] = '\0';
    }
  }
  
  jsval_t result = js_mkstr(js, base, strlen(base));
  free(path_copy);
  return result;
}

// path.dirname(path)
static jsval_t builtin_path_dirname(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "dirname() requires a path argument");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "dirname() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkstr(js, ".", 1);
  
  char *path_copy = strndup(path, path_len);
  if (!path_copy) return js_mkerr(js, "Out of memory");
  
  char *dir = dirname(path_copy);
  jsval_t result = js_mkstr(js, dir, strlen(dir));
  free(path_copy);
  return result;
}

// path.extname(path)
static jsval_t builtin_path_extname(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "extname() requires a path argument");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "extname() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkstr(js, "", 0);
  
  char *path_copy = strndup(path, path_len);
  if (!path_copy) return js_mkerr(js, "Out of memory");
  
  char *base = basename(path_copy);
  char *last_dot = strrchr(base, '.');
  
  if (last_dot && last_dot != base) {
    jsval_t result = js_mkstr(js, last_dot, strlen(last_dot));
    free(path_copy);
    return result;
  }
  
  free(path_copy);
  return js_mkstr(js, "", 0);
}

// path.join(...paths)
static jsval_t builtin_path_join(struct js *js, jsval_t *args, int nargs) {
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
    if (js_type(args[i]) == JS_STR) {
      segments[valid_segments] = js_getstr(js, args[i], &lengths[valid_segments]);
      if (segments[valid_segments] && lengths[valid_segments] > 0) {
        total_len += lengths[valid_segments] + 1; // +1 for separator
        valid_segments++;
      }
    }
  }
  
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
    if (i > 0 && pos > 0 && result[pos - 1] != PATH_SEP) {
      result[pos++] = PATH_SEP;
    }
    
    size_t start = 0;
    if (i > 0 && segments[i][0] == PATH_SEP) start = 1;
    
    size_t seg_len = lengths[i];
    if (seg_len > start && segments[i][seg_len - 1] == PATH_SEP) seg_len--;
    
    if (seg_len > start) {
      memcpy(result + pos, segments[i] + start, seg_len - start);
      pos += seg_len - start;
    }
  }
  
  result[pos] = '\0';
  
  jsval_t ret = js_mkstr(js, result, pos);
  free(result);
  free(segments);
  free(lengths);
  return ret;
}

// path.normalize(path)
static jsval_t builtin_path_normalize(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "normalize() requires a path argument");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "normalize() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkstr(js, ".", 1);
  
  char *normalized = normalize_separators(path, path_len);
  if (!normalized) return js_mkerr(js, "Out of memory");
  
  char *result = malloc(path_len + 1);
  if (!result) {
    free(normalized);
    return js_mkerr(js, "Out of memory");
  }
  
  size_t j = 0;
  int last_was_sep = 0;
  
  for (size_t i = 0; i < path_len; i++) {
    if (normalized[i] == PATH_SEP) {
      if (!last_was_sep) {
        result[j++] = PATH_SEP;
        last_was_sep = 1;
      }
    } else {
      result[j++] = normalized[i];
      last_was_sep = 0;
    }
  }
  
  if (j > 1 && result[j - 1] == PATH_SEP) j--;  
  result[j] = '\0';
  
  jsval_t ret = js_mkstr(js, result, j);
  free(normalized);
  free(result);
  return ret;
}

// path.resolve(...paths)
static jsval_t builtin_path_resolve(struct js *js, jsval_t *args, int nargs) {
  char cwd[PATH_MAX];
  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    return js_mkerr(js, "Failed to get current working directory");
  }
  
  char *result = strdup(cwd);
  if (!result) return js_mkerr(js, "Out of memory");
  
  for (int i = 0; i < nargs; i++) {
    if (js_type(args[i]) != JS_STR) continue;
    
    size_t seg_len;
    char *segment = js_getstr(js, args[i], &seg_len);
    if (!segment || seg_len == 0) continue;
    
    if (segment[0] == PATH_SEP) {
      free(result);
      result = strndup(segment, seg_len);
      if (!result) return js_mkerr(js, "Out of memory");
    } else {
      size_t result_len = strlen(result);
      size_t new_len = result_len + seg_len + 2;
      char *new_result = malloc(new_len);
      if (!new_result) {
        free(result);
        return js_mkerr(js, "Out of memory");
      }
      
      snprintf(new_result, new_len, "%s%c%.*s", result, PATH_SEP, (int)seg_len, segment);
      free(result);
      result = new_result;
    }
  }
  
  jsval_t ret = js_mkstr(js, result, strlen(result));
  free(result);
  return ret;
}

// path.relative(from, to)
static jsval_t builtin_path_relative(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 2) return js_mkerr(js, "relative() requires from and to arguments");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "relative() from must be a string");
  if (js_type(args[1]) != JS_STR) return js_mkerr(js, "relative() to must be a string");
  
  size_t from_len, to_len;
  char *from = js_getstr(js, args[0], &from_len);
  char *to = js_getstr(js, args[1], &to_len);
  
  if (!from || !to) return js_mkerr(js, "Failed to get arguments");
  if (from_len == to_len && strncmp(from, to, from_len) == 0) return js_mkstr(js, "", 0);
  
  return js_mkstr(js, to, to_len);
}

// path.isAbsolute(path)
static jsval_t builtin_path_isAbsolute(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "isAbsolute() requires a path argument");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "isAbsolute() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path || path_len == 0) return js_mkfalse();
  
#ifdef _WIN32
  if (path_len >= 2 && path[1] == ':') return js_mktrue();
  if (path_len >= 2 && path[0] == '\\' && path[1] == '\\') return js_mktrue();
  return js_mkfalse();
#else
  return path[0] == '/' ? js_mktrue() : js_mkfalse();
#endif
}

// path.parse(path)
static jsval_t builtin_path_parse(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "parse() requires a path argument");
  if (js_type(args[0]) != JS_STR) return js_mkerr(js, "parse() path must be a string");
  
  size_t path_len;
  char *path = js_getstr(js, args[0], &path_len);
  if (!path) return js_mkerr(js, "Failed to get path string");
  
  jsval_t result = js_mkobj(js);
  
  if (path_len > 0 && path[0] == PATH_SEP) {
    js_set(js, result, "root", js_mkstr(js, PATH_SEP_STR, 1));
  } else {
    js_set(js, result, "root", js_mkstr(js, "", 0));
  }
  
  char *path_copy = strndup(path, path_len);
  if (!path_copy) return js_mkerr(js, "Out of memory");
  char *dir = dirname(path_copy);
  js_set(js, result, "dir", js_mkstr(js, dir, strlen(dir)));
  free(path_copy);
  
  path_copy = strndup(path, path_len);
  if (!path_copy) return js_mkerr(js, "Out of memory");
  char *base = basename(path_copy);
  js_set(js, result, "base", js_mkstr(js, base, strlen(base)));
  
  char *last_dot = strrchr(base, '.');
  if (last_dot && last_dot != base && *(last_dot + 1) != '\0') {
    js_set(js, result, "ext", js_mkstr(js, last_dot, strlen(last_dot)));
    size_t name_len = last_dot - base;
    js_set(js, result, "name", js_mkstr(js, base, name_len));
  } else {
    js_set(js, result, "ext", js_mkstr(js, "", 0));
    js_set(js, result, "name", js_mkstr(js, base, strlen(base)));
  }
  
  free(path_copy);
  return result;
}

// path.format(pathObject)
static jsval_t builtin_path_format(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr(js, "format() requires a path object argument");
  if (js_type(args[0]) != JS_PRIV) return js_mkerr(js, "format() argument must be an object");
  
  jsval_t obj = args[0];
  
  jsval_t dir_val = js_get(js, obj, "dir");
  jsval_t root_val = js_get(js, obj, "root");
  jsval_t base_val = js_get(js, obj, "base");
  jsval_t name_val = js_get(js, obj, "name");
  jsval_t ext_val = js_get(js, obj, "ext");
  
  char result[PATH_MAX] = {0};
  size_t pos = 0;
  
  if (js_type(dir_val) == JS_STR) {
    size_t len;
    char *str = js_getstr(js, dir_val, &len);
    if (str && len > 0 && pos + len < PATH_MAX) {
      memcpy(result + pos, str, len);
      pos += len;
      if (result[pos - 1] != PATH_SEP && pos < PATH_MAX - 1) {
        result[pos++] = PATH_SEP;
      }
    }
  } else if (js_type(root_val) == JS_STR) {
    size_t len;
    char *str = js_getstr(js, root_val, &len);
    if (str && len > 0 && pos + len < PATH_MAX) {
      memcpy(result + pos, str, len);
      pos += len;
    }
  }
  
  if (js_type(base_val) == JS_STR) {
    size_t len;
    char *str = js_getstr(js, base_val, &len);
    if (str && len > 0 && pos + len < PATH_MAX) {
      memcpy(result + pos, str, len);
      pos += len;
    }
  } else {
    if (js_type(name_val) == JS_STR) {
      size_t len;
      char *str = js_getstr(js, name_val, &len);
      if (str && len > 0 && pos + len < PATH_MAX) {
        memcpy(result + pos, str, len);
        pos += len;
      }
    }
    if (js_type(ext_val) == JS_STR) {
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

jsval_t path_library(struct js *js) {
  jsval_t lib = js_mkobj(js);
  
  js_set(js, lib, "basename", js_mkfun(builtin_path_basename));
  js_set(js, lib, "dirname", js_mkfun(builtin_path_dirname));
  js_set(js, lib, "extname", js_mkfun(builtin_path_extname));
  js_set(js, lib, "join", js_mkfun(builtin_path_join));
  js_set(js, lib, "normalize", js_mkfun(builtin_path_normalize));
  js_set(js, lib, "resolve", js_mkfun(builtin_path_resolve));
  js_set(js, lib, "relative", js_mkfun(builtin_path_relative));
  js_set(js, lib, "isAbsolute", js_mkfun(builtin_path_isAbsolute));
  js_set(js, lib, "parse", js_mkfun(builtin_path_parse));
  js_set(js, lib, "format", js_mkfun(builtin_path_format));
  
  js_set(js, lib, "sep", js_mkstr(js, PATH_SEP_STR, 1));
  char delimiter_str[2] = {PATH_DELIMITER, '\0'};
  js_set(js, lib, "delimiter", js_mkstr(js, delimiter_str, 1));
  js_set(js, lib, "@@toStringTag", js_mkstr(js, "path", 4));
  js_set(js, lib, "default", lib);
  
  return lib;
}
