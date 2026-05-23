#include "resolver.h"
#include "debug.h"
#include "lockfile.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uthash.h>
#include <yyjson.h>

typedef struct {
  char *key;
  UT_hash_handle hh;
} string_hash_entry_t;

typedef struct {
  char *key;
  uint32_t index;
  UT_hash_handle hh;
} pkg_index_entry_t;

// Define standard log/timer macros if not present in debug.h
#ifndef debug_log
#define debug_log(fmt, ...) printf("[debug] " fmt "\n", ##__VA_ARGS__)
#endif

// Base64 helper decoding function for integrity checks
static bool base64_decode(const char *in, size_t in_len, uint8_t *out,
                          size_t *out_len) {
  static const int8_t table[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55, 56, 57,
      58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,
      7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
      25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,
      37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1};

  size_t i = 0, j = 0;
  uint32_t accum = 0;
  int bits = 0;
  for (i = 0; i < in_len; i++) {
    int val = table[(unsigned char)in[i]];
    if (val == -1) {
      if (in[i] == '=')
        break;
      continue;
    }
    accum = (accum << 6) | val;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (j >= *out_len)
        return false;
      out[j++] = (accum >> bits) & 0xFF;
    }
  }
  *out_len = j;
  return true;
}

static bool hex_decode(const char *in, size_t in_len, uint8_t *out,
                       size_t out_len) {
  if (in_len < out_len * 2)
    return false;
  for (size_t i = 0; i < out_len; i++) {
    char high = in[i * 2];
    char low = in[i * 2 + 1];
    int h = (high >= '0' && high <= '9')   ? (high - '0')
            : (high >= 'a' && high <= 'f') ? (high - 'a' + 10)
            : (high >= 'A' && high <= 'F') ? (high - 'A' + 10)
                                           : -1;
    int l = (low >= '0' && low <= '9')   ? (low - '0')
            : (low >= 'a' && low <= 'f') ? (low - 'a' + 10)
            : (low >= 'A' && low <= 'F') ? (low - 'A' + 10)
                                         : -1;
    if (h == -1 || l == -1)
      return false;
    out[i] = (h << 4) | l;
  }
  return true;
}

// Semver Parsing & Comparison Implementation
bool semver_parse(const char *str, semver_version_t *out) {
  memset(out, 0, sizeof(semver_version_t));
  const char *remaining = str;
  if (*remaining == 'v')
    remaining++;

  char *endptr;
  out->major = strtoull(remaining, &endptr, 10);
  if (endptr == remaining || *endptr != '.')
    return false;
  remaining = endptr + 1;

  out->minor = strtoull(remaining, &endptr, 10);
  if (endptr == remaining || *endptr != '.')
    return false;
  remaining = endptr + 1;

  out->patch = strtoull(remaining, &endptr, 10);
  if (endptr == remaining)
    return false;
  remaining = endptr;

  if (*remaining == '-') {
    remaining++;
    const char *plus = strchr(remaining, '+');
    if (plus) {
      out->prerelease = strndup(remaining, plus - remaining);
      out->build = strdup(plus + 1);
    } else {
      out->prerelease = strdup(remaining);
    }
  } else if (*remaining == '+') {
    out->build = strdup(remaining + 1);
  }

  return true;
}

static uint64_t parse_numeric(const char *s, size_t len) {
  if (len == 0)
    return 0;
  uint64_t val = 0;
  for (size_t i = 0; i < len; i++) {
    if (!isdigit((unsigned char)s[i]))
      return 0;
    val = val * 10 + (s[i] - '0');
  }
  return val;
}

static bool is_numeric(const char *s, size_t len) {
  if (len == 0)
    return false;
  for (size_t i = 0; i < len; i++) {
    if (!isdigit((unsigned char)s[i]))
      return false;
  }
  return true;
}

static int compare_identifier(const char *a, size_t a_len, const char *b,
                              size_t b_len) {
  bool a_num = is_numeric(a, a_len);
  bool b_num = is_numeric(b, b_len);

  if (a_num && b_num) {
    uint64_t an = parse_numeric(a, a_len);
    uint64_t bn = parse_numeric(b, b_len);
    if (an < bn)
      return -1;
    if (an > bn)
      return 1;
    return 0;
  }

  if (a_num)
    return -1;
  if (b_num)
    return 1;

  size_t min_len = a_len < b_len ? a_len : b_len;
  int cmp = strncmp(a, b, min_len);
  if (cmp != 0)
    return cmp < 0 ? -1 : 1;

  if (a_len < b_len)
    return -1;
  if (a_len > b_len)
    return 1;
  return 0;
}

static int order_prerelease(const char *a, const char *b) {
  const char *a_rest = a;
  const char *b_rest = b;

  while (true) {
    const char *a_dot = strchr(a_rest, '.');
    const char *b_dot = strchr(b_rest, '.');

    size_t a_len = a_dot ? (size_t)(a_dot - a_rest) : strlen(a_rest);
    size_t b_len = b_dot ? (size_t)(b_dot - b_rest) : strlen(b_rest);

    int cmp = compare_identifier(a_rest, a_len, b_rest, b_len);
    if (cmp != 0)
      return cmp;

    bool a_done = !a_dot;
    bool b_done = !b_dot;
    if (a_done && b_done)
      return 0;
    if (a_done)
      return -1;
    if (b_done)
      return 1;

    a_rest = a_dot + 1;
    b_rest = b_dot + 1;
  }
}

int semver_compare(const semver_version_t *a, const semver_version_t *b) {
  if (a->major != b->major)
    return a->major < b->major ? -1 : 1;
  if (a->minor != b->minor)
    return a->minor < b->minor ? -1 : 1;
  if (a->patch != b->patch)
    return a->patch < b->patch ? -1 : 1;

  if (!a->prerelease && b->prerelease)
    return 1;
  if (a->prerelease && !b->prerelease)
    return -1;
  if (!a->prerelease && !b->prerelease)
    return 0;

  return order_prerelease(a->prerelease, b->prerelease);
}

void semver_free(semver_version_t *v) {
  free(v->prerelease);
  free(v->build);
  memset(v, 0, sizeof(semver_version_t));
}

char *semver_format(const semver_version_t *v) {
  char buf[512];
  if (v->prerelease) {
    snprintf(buf, sizeof(buf), "%llu.%llu.%llu-%s", v->major, v->minor,
             v->patch, v->prerelease);
  } else {
    snprintf(buf, sizeof(buf), "%llu.%llu.%llu", v->major, v->minor, v->patch);
  }
  return strdup(buf);
}

// Constraint Parsing & Evaluation
bool semver_constraint_parse(const char *str, semver_constraint_t *out) {
  memset(out, 0, sizeof(semver_constraint_t));
  if (!str || strlen(str) == 0 || strcmp(str, "*") == 0 ||
      strcmp(str, "latest") == 0) {
    out->kind = CONSTRAINT_ANY;
    return true;
  }

  const char *remaining = str;

  // Find logical ORs or spaces and grab the last/first portion (standard NPM
  // range logic from resolver.zig)
  const char *or_idx = strstr(remaining, "||");
  if (or_idx) {
    remaining = or_idx + 2;
    while (*remaining == ' ')
      remaining++;
  }

  const char *space = strchr(remaining, ' ');
  char *temp_buf = NULL;
  if (space) {
    temp_buf = strndup(remaining, space - remaining);
    remaining = temp_buf;
  }

  out->kind = CONSTRAINT_EXACT;
  if (strncmp(remaining, "^", 1) == 0) {
    out->kind = CONSTRAINT_CARET;
    remaining += 1;
  } else if (strncmp(remaining, "~", 1) == 0) {
    out->kind = CONSTRAINT_TILDE;
    remaining += 1;
  } else if (strncmp(remaining, ">=", 2) == 0) {
    out->kind = CONSTRAINT_GTE;
    remaining += 2;
  } else if (strncmp(remaining, ">", 1) == 0) {
    out->kind = CONSTRAINT_GT;
    remaining += 1;
  } else if (strncmp(remaining, "<=", 2) == 0) {
    out->kind = CONSTRAINT_LTE;
    remaining += 2;
  } else if (strncmp(remaining, "<", 1) == 0) {
    out->kind = CONSTRAINT_LT;
    remaining += 1;
  } else if (strncmp(remaining, "=", 1) == 0) {
    remaining += 1;
  }

  // Handle partial versions
  int dot_count = 0;
  for (const char *c = remaining; *c; c++) {
    if (*c == '.')
      dot_count++;
  }

  if (dot_count == 0) {
    char *endptr;
    uint64_t major = strtoull(remaining, &endptr, 10);
    if (endptr != remaining) {
      out->kind =
          (out->kind == CONSTRAINT_EXACT) ? CONSTRAINT_CARET : out->kind;
      out->version.major = major;
      out->version.minor = 0;
      out->version.patch = 0;
      free(temp_buf);
      return true;
    }
  } else if (dot_count == 1) {
    char *copy = strdup(remaining);
    char *dot = strchr(copy, '.');
    *dot = '\0';
    out->version.major = strtoull(copy, NULL, 10);
    out->version.minor = strtoull(dot + 1, NULL, 10);
    out->version.patch = 0;
    free(copy);
    out->kind = (out->kind == CONSTRAINT_EXACT) ? CONSTRAINT_TILDE : out->kind;
    free(temp_buf);
    return true;
  }

  bool success = semver_parse(remaining, &out->version);
  free(temp_buf);
  return success;
}

bool semver_constraint_satisfies(const semver_constraint_t *constraint,
                                 const semver_version_t *version) {
  if (constraint->kind == CONSTRAINT_ANY)
    return true;

  int cmp = semver_compare(version, &constraint->version);

  switch (constraint->kind) {
  case CONSTRAINT_EXACT: {
    if (cmp != 0)
      return false;
    if (!constraint->version.prerelease && !version->prerelease)
      return true;
    if (!constraint->version.prerelease || !version->prerelease)
      return false;
    return strcmp(constraint->version.prerelease, version->prerelease) == 0;
  }
  case CONSTRAINT_CARET: {
    if (cmp < 0)
      return false;
    if (constraint->version.major > 0) {
      return version->major == constraint->version.major;
    } else if (constraint->version.minor > 0) {
      return version->major == 0 && version->minor == constraint->version.minor;
    } else {
      return version->major == 0 && version->minor == 0 &&
             version->patch == constraint->version.patch;
    }
  }
  case CONSTRAINT_TILDE: {
    if (cmp < 0)
      return false;
    return version->major == constraint->version.major &&
           version->minor == constraint->version.minor;
  }
  case CONSTRAINT_GTE:
    return cmp >= 0;
  case CONSTRAINT_GT:
    return cmp > 0;
  case CONSTRAINT_LTE:
    return cmp <= 0;
  case CONSTRAINT_LT:
    return cmp < 0;
  case CONSTRAINT_ANY:
    return true;
  }
  return false;
}

void semver_constraint_free(semver_constraint_t *c) {
  semver_free(&c->version);
}

// Dependency spec aliases
typedef struct {
  char *install_name;
  char *package_name;
  char *constraint;
} dependency_spec_t;

static dependency_spec_t dependency_spec(const char *install_name,
                                         const char *constraint) {
  dependency_spec_t spec;
  spec.install_name = strdup(install_name);
  if (strncmp(constraint, "npm:", 4) == 0) {
    const char *rest = constraint + 4;
    const char *at = strrchr(rest, '@');
    if (at) {
      spec.package_name = strndup(rest, at - rest);
      spec.constraint = strdup(at + 1);
    } else {
      spec.package_name = strdup(rest);
      spec.constraint = strdup("*");
    }
  } else {
    spec.package_name = strdup(install_name);
    spec.constraint = strdup(constraint);
  }
  return spec;
}

static void dependency_spec_free(dependency_spec_t *spec) {
  free(spec->install_name);
  free(spec->package_name);
  free(spec->constraint);
}

// Maps and types for internal resolver caching are now in resolver.h

// Platform Matcher
static bool matches_filter(const char *filter, const char *value) {
  char *copy = strdup(filter);
  if (!copy)
    return false;

  bool has_positive = false;
  bool matches = false;

  char *token = strtok(copy, ",");
  while (token != NULL) {
    // Trim spaces
    while (*token == ' ')
      token++;
    size_t len = strlen(token);
    while (len > 0 && token[len - 1] == ' ') {
      token[len - 1] = '\0';
      len--;
    }

    if (len > 0) {
      if (token[0] == '!') {
        if (strcmp(token + 1, value) == 0) {
          free(copy);
          return false;
        }
      } else {
        has_positive = true;
        if (strcmp(token, value) == 0) {
          matches = true;
        }
      }
    }
    token = strtok(NULL, ",");
  }
  free(copy);
  return has_positive ? matches : true;
}

static bool version_info_matches_platform(const version_info_t *self) {
#if defined(__APPLE__)
  const char *current_os = "darwin";
#elif defined(_WIN32)
  const char *current_os = "win32";
#elif defined(__linux__)
  const char *current_os = "linux";
#elif defined(__FreeBSD__)
  const char *current_os = "freebsd";
#else
  const char *current_os = "unknown";
#endif

#if defined(__aarch64__) || defined(__arm64__)
  const char *current_cpu = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  const char *current_cpu = "x64";
#elif defined(__i386__) || defined(_M_IX86)
  const char *current_cpu = "ia32";
#elif defined(__arm__) || defined(_M_ARM)
  const char *current_cpu = "arm";
#else
  const char *current_cpu = "unknown";
#endif

  const char *current_libc = NULL;
#if defined(__linux__)
#if defined(__GLIBC__)
  current_libc = "glibc";
#else
  current_libc = "musl";
#endif
#endif

  if (self->os && !matches_filter(self->os, current_os))
    return false;
  if (self->cpu && !matches_filter(self->cpu, current_cpu))
    return false;
  if (self->libc && current_libc && !matches_filter(self->libc, current_libc))
    return false;

  return true;
}

static name_value_t *parse_dependencies_map(yyjson_val *obj,
                                            uint32_t *out_count) {
  *out_count = 0;
  if (!yyjson_is_obj(obj))
    return NULL;

  size_t count = yyjson_obj_size(obj);
  if (count == 0)
    return NULL;

  name_value_t *arr = malloc(count * sizeof(name_value_t));
  if (!arr)
    return NULL;

  size_t idx = 0;
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(obj, &iter);
  yyjson_val *key;
  while ((key = yyjson_obj_iter_next(&iter))) {
    yyjson_val *val = yyjson_obj_iter_get_val(key);
    const char *name = yyjson_get_str(key);
    const char *constraint = yyjson_get_str(val);
    if (name && constraint) {
      arr[idx].name = strdup(name);
      arr[idx].constraint = strdup(constraint);
      idx++;
    }
  }
  *out_count = idx;
  return arr;
}

static char **parse_peer_meta_map(yyjson_val *obj, uint32_t *out_count) {
  *out_count = 0;
  if (!yyjson_is_obj(obj))
    return NULL;

  size_t count = yyjson_obj_size(obj);
  if (count == 0)
    return NULL;

  char **arr = malloc(count * sizeof(char *));
  if (!arr)
    return NULL;

  size_t idx = 0;
  yyjson_obj_iter iter;
  yyjson_obj_iter_init(obj, &iter);
  yyjson_val *key;
  while ((key = yyjson_obj_iter_next(&iter))) {
    yyjson_val *val = yyjson_obj_iter_get_val(key);
    const char *name = yyjson_get_str(key);
    yyjson_val *opt_val = yyjson_obj_get(val, "optional");
    if (name && yyjson_get_bool(opt_val)) {
      arr[idx++] = strdup(name);
    }
  }
  *out_count = idx;
  return arr;
}

static char *parse_filter_array(yyjson_val *arr) {
  if (!yyjson_is_arr(arr))
    return NULL;
  size_t size = yyjson_arr_size(arr);
  if (size == 0)
    return NULL;

  char buf[4096] = {0};
  size_t offset = 0;
  for (size_t i = 0; i < size; i++) {
    yyjson_val *item = yyjson_arr_get(arr, i);
    const char *str = yyjson_get_str(item);
    if (str) {
      if (offset > 0 && offset < sizeof(buf) - 2) {
        buf[offset++] = ',';
      }
      size_t len = strlen(str);
      if (offset + len < sizeof(buf) - 1) {
        memcpy(buf + offset, str, len);
        offset += len;
      }
    }
  }
  buf[offset] = '\0';
  return strdup(buf);
}

package_metadata_t parse_metadata_json(const char *name, const char *json_data,
                                       size_t json_len) {
  package_metadata_t meta = {0};
  meta.name = strdup(name);

  yyjson_read_err err;
  yyjson_doc *doc =
      yyjson_read_opts((char *)json_data, json_len, 0, NULL, &err);
  if (!doc)
    return meta;

  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(doc);
    return meta;
  }

  // dist-tags.latest
  yyjson_val *dist_tags = yyjson_obj_get(root, "dist-tags");
  if (yyjson_is_obj(dist_tags)) {
    yyjson_val *latest = yyjson_obj_get(dist_tags, "latest");
    if (yyjson_is_str(latest)) {
      meta.has_latest =
          semver_parse(yyjson_get_str(latest), &meta.dist_tag_latest);
    }
  }

  // versions
  yyjson_val *versions_obj = yyjson_obj_get(root, "versions");
  if (yyjson_is_obj(versions_obj)) {
    size_t count = yyjson_obj_size(versions_obj);
    meta.versions = calloc(count, sizeof(version_info_t));

    yyjson_obj_iter iter;
    yyjson_obj_iter_init(versions_obj, &iter);
    yyjson_val *ver_str_key;
    while ((ver_str_key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *ver_data = yyjson_obj_iter_get_val(ver_str_key);
      const char *ver_str = yyjson_get_str(ver_str_key);
      if (!yyjson_is_obj(ver_data))
        continue;

      semver_version_t version;
      if (!semver_parse(ver_str, &version))
        continue;

      yyjson_val *dist = yyjson_obj_get(ver_data, "dist");
      if (!yyjson_is_obj(dist)) {
        semver_free(&version);
        continue;
      }

      const char *tarball = yyjson_get_str(yyjson_obj_get(dist, "tarball"));
      if (!tarball) {
        semver_free(&version);
        continue;
      }

      uint8_t integrity[64] = {0};
      const char *int_str = yyjson_get_str(yyjson_obj_get(dist, "integrity"));
      if (int_str && strncmp(int_str, "sha512-", 7) == 0) {
        size_t len = 64;
        base64_decode(int_str + 7, strlen(int_str + 7), integrity, &len);
      } else {
        const char *shasum = yyjson_get_str(yyjson_obj_get(dist, "shasum"));
        if (shasum) {
          hex_decode(shasum, strlen(shasum), integrity, 20);
        }
      }

      version_info_t *vi = &meta.versions[meta.version_count++];
      vi->version = version;
      vi->version_str = strdup(ver_str);
      memcpy(vi->integrity, integrity, 64);
      vi->tarball_url = strdup(tarball);

      vi->dependencies = parse_dependencies_map(
          yyjson_obj_get(ver_data, "dependencies"), &vi->dep_count);
      vi->optional_dependencies = parse_dependencies_map(
          yyjson_obj_get(ver_data, "optionalDependencies"), &vi->opt_dep_count);
      vi->peer_dependencies = parse_dependencies_map(
          yyjson_obj_get(ver_data, "peerDependencies"), &vi->peer_dep_count);
      vi->peer_dependencies_meta =
          parse_peer_meta_map(yyjson_obj_get(ver_data, "peerDependenciesMeta"),
                              &vi->peer_meta_count);

      vi->os = parse_filter_array(yyjson_obj_get(ver_data, "os"));
      vi->cpu = parse_filter_array(yyjson_obj_get(ver_data, "cpu"));
      vi->libc = parse_filter_array(yyjson_obj_get(ver_data, "libc"));

      // bin parsing
      yyjson_val *bin_val = yyjson_obj_get(ver_data, "bin");
      if (yyjson_is_obj(bin_val)) {
        size_t bin_sz = yyjson_obj_size(bin_val);
        vi->bin = malloc(bin_sz * sizeof(name_value_t));
        yyjson_obj_iter b_iter;
        yyjson_obj_iter_init(bin_val, &b_iter);
        yyjson_val *b_key;
        while ((b_key = yyjson_obj_iter_next(&b_iter))) {
          yyjson_val *b_val = yyjson_obj_iter_get_val(b_key);
          const char *b_name = yyjson_get_str(b_key);
          const char *b_path = yyjson_get_str(b_val);
          if (b_name && b_path) {
            vi->bin[vi->bin_count].name = strdup(b_name);
            vi->bin[vi->bin_count].constraint = strdup(b_path);
            vi->bin_count++;
          }
        }
      } else if (yyjson_is_str(bin_val)) {
        const char *b_path = yyjson_get_str(bin_val);
        if (b_path) {
          vi->bin = malloc(sizeof(name_value_t));
          vi->bin[0].name = strdup(name);
          vi->bin[0].constraint = strdup(b_path);
          vi->bin_count = 1;
        }
      }
    }
  }

  yyjson_doc_free(doc);
  return meta;
}

static void package_metadata_free(package_metadata_t *meta) {
  free(meta->name);
  if (meta->has_latest)
    semver_free(&meta->dist_tag_latest);

  for (uint32_t i = 0; i < meta->version_count; i++) {
    version_info_t *vi = &meta->versions[i];
    semver_free(&vi->version);
    free(vi->version_str);
    free(vi->tarball_url);

    for (uint32_t j = 0; j < vi->dep_count; j++) {
      free(vi->dependencies[j].name);
      free(vi->dependencies[j].constraint);
    }
    free(vi->dependencies);

    for (uint32_t j = 0; j < vi->opt_dep_count; j++) {
      free(vi->optional_dependencies[j].name);
      free(vi->optional_dependencies[j].constraint);
    }
    free(vi->optional_dependencies);

    for (uint32_t j = 0; j < vi->peer_dep_count; j++) {
      free(vi->peer_dependencies[j].name);
      free(vi->peer_dependencies[j].constraint);
    }
    free(vi->peer_dependencies);

    for (uint32_t j = 0; j < vi->peer_meta_count; j++) {
      free(vi->peer_dependencies_meta[j]);
    }
    free(vi->peer_dependencies_meta);

    free(vi->os);
    free(vi->cpu);
    free(vi->libc);

    for (uint32_t j = 0; j < vi->bin_count; j++) {
      free(vi->bin[j].name);
      free(vi->bin[j].constraint);
    }
    free(vi->bin);
  }
  free(meta->versions);
}

// Uthash structs declarations
typedef struct {
  char *key;
  resolved_package_t *value;
  UT_hash_handle hh;
} resolved_entry_t;

typedef struct {
  char *key;
  semver_constraint_t *constraints;
  uint32_t count;
  uint32_t capacity;
  UT_hash_handle hh;
} constraints_entry_t;

typedef struct {
  char *key;
  UT_hash_handle hh;
} in_progress_entry_t;

struct resolver {
  string_pool_t *string_pool;
  cache_db_t *cache_db;
  fetcher_t *http;

  resolved_entry_t *resolved;
  metadata_entry_t *metadata_cache;
  constraints_entry_t *constraints;
  in_progress_entry_t *in_progress;

  void (*on_package_resolved)(const resolved_package_t *pkg, void *user_data);
  void *on_package_resolved_data;
};

void resolver_add_metadata_to_cache(resolver_t *self, const char *name,
                                    package_metadata_t meta) {
  metadata_entry_t *entry = calloc(1, sizeof(metadata_entry_t));
  entry->key = strdup(name);
  entry->value = meta;
  HASH_ADD_KEYPTR(hh, self->metadata_cache, entry->key, strlen(entry->key),
                  entry);
}

resolver_t *resolver_init(string_pool_t *pool, cache_db_t *cache_db,
                          fetcher_t *http) {
  resolver_t *self = calloc(1, sizeof(resolver_t));
  if (!self)
    return NULL;

  self->string_pool = pool;
  self->cache_db = cache_db;
  self->http = http;

  return self;
}

void resolver_deinit(resolver_t *self) {
  if (!self)
    return;

  // Free resolved packages
  resolved_entry_t *curr_r, *tmp_r;
  HASH_ITER(hh, self->resolved, curr_r, tmp_r) {
    HASH_DEL(self->resolved, curr_r);
    free(curr_r->key);
    resolved_package_t *pkg = curr_r->value;
    semver_free(&pkg->version);
    free(pkg->tarball_url);
    for (uint32_t i = 0; i < pkg->dep_count; i++) {
      free(pkg->dependencies[i].constraint);
    }
    free(pkg->dependencies);
    free(pkg->parent_path);
    free(pkg);
    free(curr_r);
  }

  // Free metadata cache
  metadata_entry_t *curr_m, *tmp_m;
  HASH_ITER(hh, self->metadata_cache, curr_m, tmp_m) {
    HASH_DEL(self->metadata_cache, curr_m);
    free(curr_m->key);
    package_metadata_free(&curr_m->value);
    free(curr_m);
  }

  // Free constraints
  constraints_entry_t *curr_c, *tmp_c;
  HASH_ITER(hh, self->constraints, curr_c, tmp_c) {
    HASH_DEL(self->constraints, curr_c);
    free(curr_c->key);
    for (uint32_t i = 0; i < curr_c->count; i++) {
      semver_constraint_free(&curr_c->constraints[i]);
    }
    free(curr_c->constraints);
    free(curr_c);
  }

  // Free in_progress
  in_progress_entry_t *curr_p, *tmp_p;
  HASH_ITER(hh, self->in_progress, curr_p, tmp_p) {
    HASH_DEL(self->in_progress, curr_p);
    free(curr_p->key);
    free(curr_p);
  }

  free(self);
}

void resolver_set_on_package_resolved(
    resolver_t *self,
    void (*callback)(const resolved_package_t *pkg, void *user_data),
    void *user_data) {
  self->on_package_resolved = callback;
  self->on_package_resolved_data = user_data;
}

// Version Selection Logic
const version_info_t *
select_best_version(const package_metadata_t *metadata,
                    const semver_constraint_t *constraint) {
  const version_info_t *best = NULL;
  for (uint32_t i = 0; i < metadata->version_count; i++) {
    const version_info_t *v = &metadata->versions[i];
    if (!version_info_matches_platform(v))
      continue;
    if (semver_constraint_satisfies(constraint, &v->version)) {
      if (!best || semver_compare(&v->version, &best->version) > 0) {
        best = v;
      }
    }
  }
  return best;
}

static const version_info_t *
select_best_version_for_constraints(const package_metadata_t *metadata,
                                    const semver_constraint_t *constraints,
                                    uint32_t count) {
  const version_info_t *best = NULL;
  for (uint32_t i = 0; i < metadata->version_count; i++) {
    const version_info_t *v = &metadata->versions[i];
    if (!version_info_matches_platform(v))
      continue;

    bool satisfies_all = true;
    for (uint32_t j = 0; j < count; j++) {
      if (!semver_constraint_satisfies(&constraints[j], &v->version)) {
        satisfies_all = false;
        break;
      }
    }

    if (satisfies_all) {
      if (!best || semver_compare(&v->version, &best->version) > 0) {
        best = v;
      }
    }
  }
  return best;
}

// Queue item structure for BFS Pass 1
typedef struct {
  char *name;
  char *constraint_str;
  char *requester;
  uint32_t depth;
} collect_item_t;

typedef struct {
  resolver_t *resolver;
  collect_item_t *collect_queue_items;
  uint32_t collect_queue_count;
  char **prefetch_queue;
  uint32_t prefetch_count;
  uint32_t prefetch_capacity;
} stream_context_t;

static void on_metadata_streaming(const char *name, const uint8_t *data,
                                  size_t len, bool has_error, void *user_data) {
  stream_context_t *ctx = user_data;
  if (has_error || !data)
    return;

  // Insert raw metadata to LMDB cache
  if (ctx->resolver->cache_db) {
    cache_db_t *db = ctx->resolver->cache_db;
    cache_db_insert_metadata(db, name, (const char *)data, len);
  }

  package_metadata_t metadata =
      parse_metadata_json(name, (const char *)data, len);

  metadata_entry_t *entry = calloc(1, sizeof(metadata_entry_t));
  entry->key = strdup(name);
  entry->value = metadata;
  HASH_ADD_KEYPTR(hh, ctx->resolver->metadata_cache, entry->key,
                  strlen(entry->key), entry);

  // Queue dependencies of the best version satisfying matching constraint in
  // collect queue
  for (uint32_t i = 0; i < ctx->collect_queue_count; i++) {
    collect_item_t *item = &ctx->collect_queue_items[i];
    dependency_spec_t spec = dependency_spec(item->name, item->constraint_str);
    if (strcmp(spec.package_name, name) != 0) {
      dependency_spec_free(&spec);
      continue;
    }

    semver_constraint_t constraint;
    if (!semver_constraint_parse(spec.constraint, &constraint)) {
      dependency_spec_free(&spec);
      continue;
    }

    const version_info_t *best = select_best_version(&metadata, &constraint);
    semver_constraint_free(&constraint);
    if (!best || !version_info_matches_platform(best)) {
      dependency_spec_free(&spec);
      continue;
    }

    // Check if sub-dependencies metadata is already loaded. If not, queue for
    // prefetch
    for (uint32_t j = 0; j < best->dep_count; j++) {
      dependency_spec_t dep_spec = dependency_spec(
          best->dependencies[j].name, best->dependencies[j].constraint);

      metadata_entry_t *m_cache = NULL;
      HASH_FIND_STR(ctx->resolver->metadata_cache, dep_spec.package_name,
                    m_cache);
      if (!m_cache) {
        bool already_queued = false;
        for (uint32_t k = 0; k < ctx->prefetch_count; k++) {
          if (strcmp(ctx->prefetch_queue[k], dep_spec.package_name) == 0) {
            already_queued = true;
            break;
          }
        }
        if (!already_queued) {
          if (ctx->prefetch_count >= ctx->prefetch_capacity) {
            ctx->prefetch_capacity =
                ctx->prefetch_capacity == 0 ? 16 : ctx->prefetch_capacity * 2;
            ctx->prefetch_queue = realloc(
                ctx->prefetch_queue, ctx->prefetch_capacity * sizeof(char *));
          }
          ctx->prefetch_queue[ctx->prefetch_count++] =
              strdup(dep_spec.package_name);
        }
      }
      dependency_spec_free(&dep_spec);
    }
    dependency_spec_free(&spec);
    break;
  }
}

static uint32_t
count_satisfied_constraints(const package_metadata_t *meta,
                            const version_info_t *v,
                            constraints_entry_t *constraint_list) {
  uint32_t score = 0;
  for (uint32_t i = 0; i < constraint_list->count; i++) {
    if (semver_constraint_satisfies(&constraint_list->constraints[i],
                                    &v->version)) {
      score++;
    }
  }
  return score;
}

// StringHashMap replacement using uthash
typedef struct {
  char *key; // install_name
  const version_info_t *value;
  UT_hash_handle hh;
} optimal_versions_entry_t;

// resolved package install path helper
static char *resolved_package_install_path(const resolved_package_t *pkg) {
  if (pkg->parent_path && strlen(pkg->parent_path) > 0) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/node_modules/%s", pkg->parent_path,
             pkg->name.ptr);
    return strdup(path);
  }
  return strdup(pkg->name.ptr);
}

// Helper to create a resolved package
static resolved_package_t *
create_resolved_package(resolver_t *self, const dependency_spec_t *spec,
                        const version_info_t *best, uint32_t depth, bool direct,
                        const char *parent_path) {
  resolved_package_t *pkg = calloc(1, sizeof(resolved_package_t));
  if (!pkg)
    return NULL;

  pkg->name = string_pool_intern(self->string_pool, spec->install_name,
                                 strlen(spec->install_name));
  pkg->version = best->version;
  if (best->version.prerelease)
    pkg->version.prerelease = strdup(best->version.prerelease);
  if (best->version.build)
    pkg->version.build = strdup(best->version.build);

  memcpy(pkg->integrity, best->integrity, 64);
  pkg->tarball_url = strdup(best->tarball_url);
  pkg->depth = depth;
  pkg->direct = direct;
  pkg->parent_path = parent_path ? strdup(parent_path) : NULL;
  pkg->has_bin = best->bin_count > 0;

  pkg->dep_capacity =
      best->dep_count + best->opt_dep_count + best->peer_dep_count;
  if (pkg->dep_capacity > 0) {
    pkg->dependencies = calloc(pkg->dep_capacity, sizeof(resolved_dep_t));
  }

  // Standard dependencies
  for (uint32_t i = 0; i < best->dep_count; i++) {
    resolved_dep_t *dep = &pkg->dependencies[pkg->dep_count++];
    dep->name =
        string_pool_intern(self->string_pool, best->dependencies[i].name,
                           strlen(best->dependencies[i].name));
    dep->constraint = strdup(best->dependencies[i].constraint);
  }

  // Optional dependencies (verify platform match first if metadata available)
  for (uint32_t i = 0; i < best->opt_dep_count; i++) {
    dependency_spec_t opt_spec =
        dependency_spec(best->optional_dependencies[i].name,
                        best->optional_dependencies[i].constraint);

    metadata_entry_t *opt_meta = NULL;
    HASH_FIND_STR(self->metadata_cache, opt_spec.package_name, opt_meta);
    if (opt_meta) {
      semver_constraint_t opt_con;
      if (semver_constraint_parse(opt_spec.constraint, &opt_con)) {
        const version_info_t *opt_best =
            select_best_version(&opt_meta->value, &opt_con);
        semver_constraint_free(&opt_con);
        if (opt_best && !version_info_matches_platform(opt_best)) {
          dependency_spec_free(&opt_spec);
          continue;
        }
      }
    }

    resolved_dep_t *dep = &pkg->dependencies[pkg->dep_count++];
    dep->name = string_pool_intern(self->string_pool,
                                   best->optional_dependencies[i].name,
                                   strlen(best->optional_dependencies[i].name));
    dep->constraint = strdup(best->optional_dependencies[i].constraint);
    dep->flags.optional = true;

    dependency_spec_free(&opt_spec);
  }

  // Peer dependencies
  for (uint32_t i = 0; i < best->peer_dep_count; i++) {
    bool excluded = false;
    for (uint32_t j = 0; j < best->peer_meta_count; j++) {
      if (strcmp(best->peer_dependencies_meta[j],
                 best->peer_dependencies[i].name) == 0) {
        excluded = true;
        break;
      }
    }
    if (excluded)
      continue;

    resolved_dep_t *dep = &pkg->dependencies[pkg->dep_count++];
    dep->name =
        string_pool_intern(self->string_pool, best->peer_dependencies[i].name,
                           strlen(best->peer_dependencies[i].name));
    dep->constraint = strdup(best->peer_dependencies[i].constraint);
    dep->flags.peer = true;
  }

  return pkg;
}

static resolved_package_t *
resolve_single_with_optimal(resolver_t *self, const char *name,
                            const char *constraint_str, uint32_t depth,
                            bool direct, const char *parent_path,
                            optimal_versions_entry_t *optimal_versions) {
  dependency_spec_t spec = dependency_spec(name, constraint_str);
  semver_constraint_t constraint;
  if (!semver_constraint_parse(spec.constraint, &constraint)) {
    dependency_spec_free(&spec);
    return NULL;
  }

  resolved_entry_t *existing = NULL;
  HASH_FIND_STR(self->resolved, spec.install_name, existing);
  if (existing) {
    if (semver_constraint_satisfies(&constraint, &existing->value->version)) {
      if (direct)
        existing->value->direct = true;
      if (depth < existing->value->depth)
        existing->value->depth = depth;
      semver_constraint_free(&constraint);
      dependency_spec_free(&spec);
      return existing->value;
    }

    if (parent_path) {
      metadata_entry_t *meta_entry = NULL;
      HASH_FIND_STR(self->metadata_cache, spec.package_name, meta_entry);
      if (meta_entry) {
        const version_info_t *nested_best =
            select_best_version(&meta_entry->value, &constraint);
        if (nested_best && version_info_matches_platform(nested_best)) {
          char nested_key[4096];
          snprintf(nested_key, sizeof(nested_key), "%s/node_modules/%s",
                   parent_path, spec.install_name);

          resolved_entry_t *n_existing = NULL;
          HASH_FIND_STR(self->resolved, nested_key, n_existing);
          if (n_existing) {
            semver_constraint_free(&constraint);
            dependency_spec_free(&spec);
            return n_existing->value;
          }

          resolved_package_t *pkg = create_resolved_package(
              self, &spec, nested_best, depth, false, parent_path);
          if (pkg) {
            resolved_entry_t *entry = calloc(1, sizeof(resolved_entry_t));
            entry->key = strdup(nested_key);
            entry->value = pkg;
            HASH_ADD_KEYPTR(hh, self->resolved, entry->key, strlen(entry->key),
                            entry);
            if (self->on_package_resolved) {
              self->on_package_resolved(pkg, self->on_package_resolved_data);
            }
          }
          semver_constraint_free(&constraint);
          dependency_spec_free(&spec);
          return pkg;
        }
      }
    }
    semver_constraint_free(&constraint);
    dependency_spec_free(&spec);
    return existing->value;
  }

  metadata_entry_t *meta_entry = NULL;
  HASH_FIND_STR(self->metadata_cache, spec.package_name, meta_entry);
  if (!meta_entry) {
    semver_constraint_free(&constraint);
    dependency_spec_free(&spec);
    return NULL;
  }

  const version_info_t *version_info = NULL;
  optimal_versions_entry_t *opt_entry = NULL;
  HASH_FIND_STR(optimal_versions, spec.install_name, opt_entry);
  if (opt_entry) {
    if (semver_constraint_satisfies(&constraint, &opt_entry->value->version) &&
        version_info_matches_platform(opt_entry->value)) {
      version_info = opt_entry->value;
    }
  }
  if (!version_info) {
    version_info = select_best_version(&meta_entry->value, &constraint);
  }

  semver_constraint_free(&constraint);
  if (!version_info || !version_info_matches_platform(version_info)) {
    dependency_spec_free(&spec);
    return NULL;
  }

  // Register constraint in the resolver constraints map
  constraints_entry_t *cons_entry = NULL;
  HASH_FIND_STR(self->constraints, spec.install_name, cons_entry);
  if (!cons_entry) {
    cons_entry = calloc(1, sizeof(constraints_entry_t));
    cons_entry->key = strdup(spec.install_name);
    HASH_ADD_KEYPTR(hh, self->constraints, cons_entry->key,
                    strlen(cons_entry->key), cons_entry);
  }
  if (cons_entry->count >= cons_entry->capacity) {
    cons_entry->capacity =
        cons_entry->capacity == 0 ? 4 : cons_entry->capacity * 2;
    cons_entry->constraints =
        realloc(cons_entry->constraints,
                cons_entry->capacity * sizeof(semver_constraint_t));
  }
  semver_constraint_t new_con;
  semver_constraint_parse(spec.constraint, &new_con);
  cons_entry->constraints[cons_entry->count++] = new_con;

  // NPM hoisting: when there is no existing root-level entry (confirmed by
  // the HASH_FIND_STR above), hoist this package to root regardless of what
  // parent_path requested it.  This makes the package reachable from any
  // nested consumer by walking up the node_modules ancestor chain, matching
  // the behaviour of npm v3+ flat installs.  Nesting (keeping parent_path)
  // only happens in the conflict branch above when an incompatible version is
  // already installed at root.
  const char *effective_parent =
      (parent_path && parent_path[0]) ? NULL : parent_path;

  resolved_package_t *pkg = create_resolved_package(
      self, &spec, version_info, depth, direct, effective_parent);
  if (pkg) {
    resolved_entry_t *entry = calloc(1, sizeof(resolved_entry_t));
    entry->key = strdup(spec.install_name);
    entry->value = pkg;
    HASH_ADD_KEYPTR(hh, self->resolved, entry->key, strlen(entry->key), entry);
    if (self->on_package_resolved) {
      self->on_package_resolved(pkg, self->on_package_resolved_data);
    }
  }
  dependency_spec_free(&spec);
  return pkg;
}

// Pass 2 queue item
typedef struct {
  char *name;
  char *constraint;
  uint32_t depth;
  bool direct;
  char *parent_name;
} work_item_t;

bool resolver_resolve_from_package_json(resolver_t *self, const char *path) {
  yyjson_read_err err;
  yyjson_doc *doc = yyjson_read_file(path, 0, NULL, &err);
  if (!doc)
    return false;

  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root)) {
    yyjson_doc_free(doc);
    return false;
  }

  fetcher_initiate_tarball_connections_async(self->http);

  // BFS Pass 1: Collect Constraints & metadata
  uint32_t collect_count = 0;
  uint32_t collect_capacity = 32;
  collect_item_t *collect_queue =
      malloc(collect_capacity * sizeof(collect_item_t));

  yyjson_val *deps_obj = yyjson_obj_get(root, "dependencies");
  if (yyjson_is_obj(deps_obj)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(deps_obj, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *val = yyjson_obj_iter_get_val(key);
      if (collect_count >= collect_capacity) {
        collect_capacity *= 2;
        collect_queue =
            realloc(collect_queue, collect_capacity * sizeof(collect_item_t));
      }
      collect_queue[collect_count].name = strdup(yyjson_get_str(key));
      collect_queue[collect_count].constraint_str = strdup(yyjson_get_str(val));
      collect_queue[collect_count].requester = strdup("root");
      collect_queue[collect_count].depth = 0;
      collect_count++;
    }
  }

  yyjson_val *dev_deps_obj = yyjson_obj_get(root, "devDependencies");
  if (yyjson_is_obj(dev_deps_obj)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(dev_deps_obj, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *val = yyjson_obj_iter_get_val(key);
      if (collect_count >= collect_capacity) {
        collect_capacity *= 2;
        collect_queue =
            realloc(collect_queue, collect_capacity * sizeof(collect_item_t));
      }
      collect_queue[collect_count].name = strdup(yyjson_get_str(key));
      collect_queue[collect_count].constraint_str = strdup(yyjson_get_str(val));
      collect_queue[collect_count].requester = strdup("root");
      collect_queue[collect_count].depth = 0;
      collect_count++;
    }
  }

  // Seen constraints set to avoid cycles in pass 1
  string_hash_entry_t *seen_collect = NULL;

  uint32_t collect_level = 0;
  while (collect_count > 0) {
    uint32_t to_fetch_capacity = collect_count;
    uint32_t to_fetch_count = 0;
    char **to_fetch = malloc(to_fetch_capacity * sizeof(char *));

    for (uint32_t i = 0; i < collect_count; i++) {
      dependency_spec_t spec = dependency_spec(collect_queue[i].name,
                                               collect_queue[i].constraint_str);

      metadata_entry_t *m_cache = NULL;
      HASH_FIND_STR(self->metadata_cache, spec.package_name, m_cache);
      if (!m_cache) {
        bool loaded_from_disk = false;
        if (self->cache_db) {
          char *json_data =
              cache_db_lookup_metadata(self->cache_db, spec.package_name);
          if (json_data) {
            package_metadata_t metadata = parse_metadata_json(
                spec.package_name, json_data, strlen(json_data));
            free(json_data);

            metadata_entry_t *entry = calloc(1, sizeof(metadata_entry_t));
            entry->key = strdup(spec.package_name);
            entry->value = metadata;
            HASH_ADD_KEYPTR(hh, self->metadata_cache, entry->key,
                            strlen(entry->key), entry);
            loaded_from_disk = true;
          }
        }

        if (!loaded_from_disk) {
          bool already_listed = false;
          for (uint32_t j = 0; j < to_fetch_count; j++) {
            if (strcmp(to_fetch[j], spec.package_name) == 0) {
              already_listed = true;
              break;
            }
          }
          if (!already_listed) {
            to_fetch[to_fetch_count++] = strdup(spec.package_name);
          }
        }
      }
      dependency_spec_free(&spec);
    }

    uint32_t next_collect_count = 0;
    uint32_t next_collect_capacity = 32;
    collect_item_t *next_collect =
        malloc(next_collect_capacity * sizeof(collect_item_t));

    uint32_t prefetch_capacity = 32;
    uint32_t prefetch_count = 0;
    char **prefetch_queue = malloc(prefetch_capacity * sizeof(char *));

    stream_context_t stream_ctx = {.resolver = self,
                                   .collect_queue_items = collect_queue,
                                   .collect_queue_count = collect_count,
                                   .prefetch_queue = prefetch_queue,
                                   .prefetch_count = prefetch_count,
                                   .prefetch_capacity = prefetch_capacity};

    if (to_fetch_count > 0) {
      fetcher_fetch_metadata_streaming(
          self->http, (const char *const *)to_fetch, to_fetch_count,
          on_metadata_streaming, &stream_ctx);

      // Update prefetched pointers if resized inside callback
      prefetch_queue = stream_ctx.prefetch_queue;
      prefetch_count = stream_ctx.prefetch_count;
      prefetch_capacity = stream_ctx.prefetch_capacity;

      if (prefetch_count > 0) {
        fetcher_fetch_metadata_streaming(
            self->http, (const char *const *)prefetch_queue, prefetch_count,
            on_metadata_streaming, &stream_ctx);
      }
    }

    // Free to_fetch
    for (uint32_t i = 0; i < to_fetch_count; i++)
      free(to_fetch[i]);
    free(to_fetch);
    for (uint32_t i = 0; i < prefetch_count; i++)
      free(prefetch_queue[i]);
    free(prefetch_queue);

    for (uint32_t i = 0; i < collect_count; i++) {
      char seen_key[4096];
      snprintf(seen_key, sizeof(seen_key), "%s@%s@%s", collect_queue[i].name,
               collect_queue[i].constraint_str, collect_queue[i].requester);

      string_hash_entry_t *seen_entry = NULL;
      HASH_FIND_STR(seen_collect, seen_key, seen_entry);
      if (seen_entry)
        continue;

      seen_entry = calloc(1, sizeof(*seen_entry));
      seen_entry->key = strdup(seen_key);
      HASH_ADD_KEYPTR(hh, seen_collect, seen_entry->key,
                      strlen(seen_entry->key), seen_entry);

      dependency_spec_t spec = dependency_spec(collect_queue[i].name,
                                               collect_queue[i].constraint_str);
      semver_constraint_t constraint;
      if (!semver_constraint_parse(spec.constraint, &constraint)) {
        dependency_spec_free(&spec);
        continue;
      }

      // Save constraint
      constraints_entry_t *cons_entry = NULL;
      HASH_FIND_STR(self->constraints, spec.install_name, cons_entry);
      if (!cons_entry) {
        cons_entry = calloc(1, sizeof(constraints_entry_t));
        cons_entry->key = strdup(spec.install_name);
        HASH_ADD_KEYPTR(hh, self->constraints, cons_entry->key,
                        strlen(cons_entry->key), cons_entry);
      }
      if (cons_entry->count >= cons_entry->capacity) {
        cons_entry->capacity =
            cons_entry->capacity == 0 ? 4 : cons_entry->capacity * 2;
        cons_entry->constraints =
            realloc(cons_entry->constraints,
                    cons_entry->capacity * sizeof(semver_constraint_t));
      }
      semver_constraint_parse(spec.constraint,
                              &cons_entry->constraints[cons_entry->count++]);

      metadata_entry_t *meta_entry = NULL;
      HASH_FIND_STR(self->metadata_cache, spec.package_name, meta_entry);
      if (meta_entry) {
        const version_info_t *best =
            select_best_version(&meta_entry->value, &constraint);
        if (best && version_info_matches_platform(best)) {
          // Standard dependencies
          for (uint32_t j = 0; j < best->dep_count; j++) {
            if (next_collect_count >= next_collect_capacity) {
              next_collect_capacity *= 2;
              next_collect = realloc(next_collect, next_collect_capacity *
                                                       sizeof(collect_item_t));
            }
            next_collect[next_collect_count].name =
                strdup(best->dependencies[j].name);
            next_collect[next_collect_count].constraint_str =
                strdup(best->dependencies[j].constraint);
            next_collect[next_collect_count].requester =
                strdup(collect_queue[i].name);
            next_collect[next_collect_count].depth = collect_queue[i].depth + 1;
            next_collect_count++;
          }
          // Optional dependencies
          for (uint32_t j = 0; j < best->opt_dep_count; j++) {
            dependency_spec_t opt_spec =
                dependency_spec(best->optional_dependencies[j].name,
                                best->optional_dependencies[j].constraint);
            metadata_entry_t *opt_meta = NULL;
            HASH_FIND_STR(self->metadata_cache, opt_spec.package_name,
                          opt_meta);
            if (opt_meta) {
              semver_constraint_t opt_con;
              if (semver_constraint_parse(opt_spec.constraint, &opt_con)) {
                const version_info_t *opt_best =
                    select_best_version(&opt_meta->value, &opt_con);
                semver_constraint_free(&opt_con);
                if (opt_best && !version_info_matches_platform(opt_best)) {
                  dependency_spec_free(&opt_spec);
                  continue;
                }
              }
            }
            if (next_collect_count >= next_collect_capacity) {
              next_collect_capacity *= 2;
              next_collect = realloc(next_collect, next_collect_capacity *
                                                       sizeof(collect_item_t));
            }
            next_collect[next_collect_count].name =
                strdup(best->optional_dependencies[j].name);
            next_collect[next_collect_count].constraint_str =
                strdup(best->optional_dependencies[j].constraint);
            next_collect[next_collect_count].requester =
                strdup(collect_queue[i].name);
            next_collect[next_collect_count].depth = collect_queue[i].depth + 1;
            next_collect_count++;
            dependency_spec_free(&opt_spec);
          }
          // Peer dependencies
          for (uint32_t j = 0; j < best->peer_dep_count; j++) {
            bool excluded = false;
            for (uint32_t k = 0; k < best->peer_meta_count; k++) {
              if (strcmp(best->peer_dependencies_meta[k],
                         best->peer_dependencies[j].name) == 0) {
                excluded = true;
                break;
              }
            }
            if (excluded)
              continue;

            if (next_collect_count >= next_collect_capacity) {
              next_collect_capacity *= 2;
              next_collect = realloc(next_collect, next_collect_capacity *
                                                       sizeof(collect_item_t));
            }
            next_collect[next_collect_count].name =
                strdup(best->peer_dependencies[j].name);
            next_collect[next_collect_count].constraint_str =
                strdup(best->peer_dependencies[j].constraint);
            next_collect[next_collect_count].requester =
                strdup(collect_queue[i].name);
            next_collect[next_collect_count].depth = collect_queue[i].depth + 1;
            next_collect_count++;
          }
        }
      }
      semver_constraint_free(&constraint);
      dependency_spec_free(&spec);
    }

    // Free old queue items
    for (uint32_t i = 0; i < collect_count; i++) {
      free(collect_queue[i].name);
      free(collect_queue[i].constraint_str);
      free(collect_queue[i].requester);
    }
    free(collect_queue);

    collect_queue = next_collect;
    collect_count = next_collect_count;
    collect_capacity = next_collect_capacity;
    collect_level++;
  }

  // Free seen collect
  string_hash_entry_t *curr_sc, *tmp_sc;
  HASH_ITER(hh, seen_collect, curr_sc, tmp_sc) {
    HASH_DEL(seen_collect, curr_sc);
    free(curr_sc->key);
    free(curr_sc);
  }

  // Compute optimal versions
  optimal_versions_entry_t *optimal_versions = NULL;
  constraints_entry_t *curr_c, *tmp_c;
  HASH_ITER(hh, self->constraints, curr_c, tmp_c) {
    if (curr_c->count == 0)
      continue;

    metadata_entry_t *meta = NULL;
    // Look up via any of the constraints which has the package name
    // (all constraints in curr_c point to the same install_name, which
    // translates to a package_name)
    dependency_spec_t spec = dependency_spec(curr_c->key, "");
    HASH_FIND_STR(self->metadata_cache, spec.package_name, meta);
    dependency_spec_free(&spec);

    if (meta) {
      const version_info_t *best = select_best_version_for_constraints(
          &meta->value, curr_c->constraints, curr_c->count);
      if (!best) {
        // Fallback score-based selector
        best = NULL;
        int64_t best_score = -1;
        bool want_prerelease = false;
        for (uint32_t j = 0; j < curr_c->count; j++) {
          if (curr_c->constraints[j].version.prerelease) {
            want_prerelease = true;
            break;
          }
        }
        for (uint32_t k = 0; k < meta->value.version_count; k++) {
          const version_info_t *v = &meta->value.versions[k];
          if (v->version.prerelease && !want_prerelease)
            continue;
          if (!version_info_matches_platform(v))
            continue;

          int64_t score = 0;
          for (uint32_t j = 0; j < curr_c->count; j++) {
            if (semver_constraint_satisfies(&curr_c->constraints[j],
                                            &v->version)) {
              score += 100; // simpler weight
            }
          }
          if (score > best_score ||
              (score == best_score && best &&
               semver_compare(&v->version, &best->version) > 0)) {
            best = v;
            best_score = score;
          }
        }
      }

      if (best) {
        optimal_versions_entry_t *opt =
            calloc(1, sizeof(optimal_versions_entry_t));
        opt->key = strdup(curr_c->key);
        opt->value = best;
        HASH_ADD_KEYPTR(hh, optimal_versions, opt->key, strlen(opt->key), opt);
      }
    }
  }

  // Pass 2: BFS queue
  uint32_t work_count = 0;
  uint32_t work_capacity = 32;
  work_item_t *work_queue = malloc(work_capacity * sizeof(work_item_t));

  if (yyjson_is_obj(deps_obj)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(deps_obj, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *val = yyjson_obj_iter_get_val(key);
      if (work_count >= work_capacity) {
        work_capacity *= 2;
        work_queue = realloc(work_queue, work_capacity * sizeof(work_item_t));
      }
      work_queue[work_count].name = strdup(yyjson_get_str(key));
      work_queue[work_count].constraint = strdup(yyjson_get_str(val));
      work_queue[work_count].depth = 0;
      work_queue[work_count].direct = true;
      work_queue[work_count].parent_name = NULL;
      work_count++;
    }
  }
  if (yyjson_is_obj(dev_deps_obj)) {
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(dev_deps_obj, &iter);
    yyjson_val *key;
    while ((key = yyjson_obj_iter_next(&iter))) {
      yyjson_val *val = yyjson_obj_iter_get_val(key);
      if (work_count >= work_capacity) {
        work_capacity *= 2;
        work_queue = realloc(work_queue, work_capacity * sizeof(work_item_t));
      }
      work_queue[work_count].name = strdup(yyjson_get_str(key));
      work_queue[work_count].constraint = strdup(yyjson_get_str(val));
      work_queue[work_count].depth = 0;
      work_queue[work_count].direct = true;
      work_queue[work_count].parent_name = NULL;
      work_count++;
    }
  }

  string_hash_entry_t *processed_pass2 = NULL;

  uint32_t pass2_level = 0;
  while (work_count > 0) {
    uint32_t next_work_count = 0;
    uint32_t next_work_capacity = 32;
    work_item_t *next_work = malloc(next_work_capacity * sizeof(work_item_t));

    // Collect any packages whose metadata we still need before this wave
    {
      uint32_t missing_capacity = 16;
      uint32_t missing_count = 0;
      char **missing = malloc(missing_capacity * sizeof(char *));

      for (uint32_t i = 0; i < work_count; i++) {
        dependency_spec_t spec =
            dependency_spec(work_queue[i].name, work_queue[i].constraint);
        metadata_entry_t *m = NULL;
        HASH_FIND_STR(self->metadata_cache, spec.package_name, m);
        if (!m) {
          // Check LMDB cache first
          bool loaded = false;
          if (self->cache_db) {
            char *json_data =
                cache_db_lookup_metadata(self->cache_db, spec.package_name);
            if (json_data) {
              package_metadata_t metadata = parse_metadata_json(
                  spec.package_name, json_data, strlen(json_data));
              free(json_data);
              metadata_entry_t *entry = calloc(1, sizeof(metadata_entry_t));
              entry->key = strdup(spec.package_name);
              entry->value = metadata;
              HASH_ADD_KEYPTR(hh, self->metadata_cache, entry->key,
                              strlen(entry->key), entry);
              loaded = true;
            }
          }
          if (!loaded) {
            bool already_queued = false;
            for (uint32_t k = 0; k < missing_count; k++) {
              if (strcmp(missing[k], spec.package_name) == 0) {
                already_queued = true;
                break;
              }
            }
            if (!already_queued) {
              if (missing_count >= missing_capacity) {
                missing_capacity *= 2;
                missing = realloc(missing, missing_capacity * sizeof(char *));
              }
              missing[missing_count++] = strdup(spec.package_name);
            }
          }
        }
        dependency_spec_free(&spec);
      }

      if (missing_count > 0) {
        // Stream-fetch all missing metadata in one batch
        stream_context_t miss_ctx = {
            .resolver = self,
            .collect_queue_items = NULL,
            .collect_queue_count = 0,
            .prefetch_queue = NULL,
            .prefetch_count = 0,
            .prefetch_capacity = 0,
        };
        fetcher_fetch_metadata_streaming(
            self->http, (const char *const *)missing, missing_count,
            on_metadata_streaming, &miss_ctx);
        free(miss_ctx.prefetch_queue);
        for (uint32_t i = 0; i < missing_count; i++)
          free(missing[i]);
      }
      free(missing);
    }

    for (uint32_t i = 0; i < work_count; i++) {
      char key_str[4096];
      snprintf(key_str, sizeof(key_str), "%s|%s@%s",
               work_queue[i].parent_name ? work_queue[i].parent_name : "",
               work_queue[i].name, work_queue[i].constraint);

      string_hash_entry_t *proc_entry = NULL;
      HASH_FIND_STR(processed_pass2, key_str, proc_entry);
      if (proc_entry)
        continue;

      proc_entry = calloc(1, sizeof(*proc_entry));
      proc_entry->key = strdup(key_str);
      HASH_ADD_KEYPTR(hh, processed_pass2, proc_entry->key,
                      strlen(proc_entry->key), proc_entry);

      resolved_package_t *pkg = resolve_single_with_optimal(
          self, work_queue[i].name, work_queue[i].constraint,
          work_queue[i].depth, work_queue[i].direct, work_queue[i].parent_name,
          optimal_versions);
      if (!pkg)
        continue;

      char *install_path = resolved_package_install_path(pkg);
      for (uint32_t j = 0; j < pkg->dep_count; j++) {
        char dep_key[4096];
        snprintf(dep_key, sizeof(dep_key), "%s|%s@%s", install_path,
                 pkg->dependencies[j].name.ptr,
                 pkg->dependencies[j].constraint);

        string_hash_entry_t *dep_proc = NULL;
        HASH_FIND_STR(processed_pass2, dep_key, dep_proc);
        if (!dep_proc) {
          if (next_work_count >= next_work_capacity) {
            next_work_capacity *= 2;
            next_work =
                realloc(next_work, next_work_capacity * sizeof(work_item_t));
          }
          next_work[next_work_count].name =
              strdup(pkg->dependencies[j].name.ptr);
          next_work[next_work_count].constraint =
              strdup(pkg->dependencies[j].constraint);
          next_work[next_work_count].depth = work_queue[i].depth + 1;
          next_work[next_work_count].direct = false;
          next_work[next_work_count].parent_name = strdup(install_path);
          next_work_count++;
        }
      }
      free(install_path);
    }

    // Free old work items
    for (uint32_t i = 0; i < work_count; i++) {
      free(work_queue[i].name);
      free(work_queue[i].constraint);
      free(work_queue[i].parent_name);
    }
    free(work_queue);

    work_queue = next_work;
    work_count = next_work_count;
    work_capacity = next_work_capacity;
    pass2_level++;
  }

  // Free processed_pass2
  string_hash_entry_t *curr_p2, *tmp_p2;
  HASH_ITER(hh, processed_pass2, curr_p2, tmp_p2) {
    HASH_DEL(processed_pass2, curr_p2);
    free(curr_p2->key);
    free(curr_p2);
  }

  // Free optimal versions
  optimal_versions_entry_t *curr_opt, *tmp_opt;
  HASH_ITER(hh, optimal_versions, curr_opt, tmp_opt) {
    HASH_DEL(optimal_versions, curr_opt);
    free(curr_opt->key);
    free(curr_opt);
  }

  yyjson_doc_free(doc);
  return true;
}

bool resolver_write_lockfile(resolver_t *self, const char *path) {
  lockfile_writer_t writer;
  lockfile_writer_init(&writer);

  // Create mapping of resolved packages install paths to indexes
  pkg_index_entry_t *pkg_indices = NULL;

  uint32_t idx = 0;
  resolved_entry_t *curr_r, *tmp_r;
  HASH_ITER(hh, self->resolved, curr_r, tmp_r) {
    char *install_path = resolved_package_install_path(curr_r->value);

    pkg_index_entry_t *index_entry = calloc(1, sizeof(pkg_index_entry_t));
    index_entry->key = install_path; // ownership transferred
    index_entry->index = idx++;
    HASH_ADD_KEYPTR(hh, pkg_indices, index_entry->key, strlen(index_entry->key),
                    index_entry);
  }

  HASH_ITER(hh, self->resolved, curr_r, tmp_r) {
    resolved_package_t *pkg = curr_r->value;

    char *install_path = resolved_package_install_path(pkg);

    lockfile_string_ref_t name_ref = lockfile_writer_intern_string(
        &writer, pkg->name.ptr, strlen(pkg->name.ptr));
    lockfile_string_ref_t url_ref = lockfile_writer_intern_string(
        &writer, pkg->tarball_url, strlen(pkg->tarball_url));

    char *ver_fmt = semver_format(&pkg->version);
    // Prerelease ref
    lockfile_string_ref_t prerelease_ref = lockfile_string_ref_empty();
    if (pkg->version.prerelease) {
      prerelease_ref = lockfile_writer_intern_string(
          &writer, pkg->version.prerelease, strlen(pkg->version.prerelease));
    }

    lockfile_string_ref_t parent_ref = lockfile_string_ref_empty();
    if (pkg->parent_path) {
      parent_ref = lockfile_writer_intern_string(&writer, pkg->parent_path,
                                                 strlen(pkg->parent_path));
    }

    uint32_t deps_start = (uint32_t)writer.dependency_count;
    uint32_t deps_written = 0;

    for (uint32_t i = 0; i < pkg->dep_count; i++) {
      const char *dep_name = pkg->dependencies[i].name.ptr;

      pkg_index_entry_t *found = NULL;
      // Try resolving dependency package under root first
      HASH_FIND_STR(pkg_indices, dep_name, found);
      if (!found) {
        // Try nested dependency package name
        char nested[4096];
        snprintf(nested, sizeof(nested), "%s/node_modules/%s", install_path,
                 dep_name);
        HASH_FIND_STR(pkg_indices, nested, found);
      }

      if (found) {
        lockfile_string_ref_t constraint_ref = lockfile_writer_intern_string(
            &writer, pkg->dependencies[i].constraint,
            strlen(pkg->dependencies[i].constraint));
        lockfile_dependency_t dep = {
            .package_index = found->index,
            .constraint = constraint_ref,
            .flags = {.peer = pkg->dependencies[i].flags.peer,
                      .optional = pkg->dependencies[i].flags.optional}};
        lockfile_writer_add_dependency(&writer, dep);
        deps_written++;
      }
    }

    lockfile_package_t lock_pkg = {
        .name = name_ref,
        .version_major = pkg->version.major,
        .version_minor = pkg->version.minor,
        .version_patch = pkg->version.patch,
        .prerelease = prerelease_ref,
        .tarball_url = url_ref,
        .parent_path = parent_ref,
        .deps_start = deps_start,
        .deps_count = deps_written,
        .flags = {.direct = pkg->direct, .has_bin = pkg->has_bin}};
    memcpy(lock_pkg.integrity, pkg->integrity, 64);
    lockfile_writer_add_package(&writer, lock_pkg);

    free(ver_fmt);
    free(install_path);
  }

  // Free indices
  pkg_index_entry_t *curr_idx, *tmp_idx;
  HASH_ITER(hh, pkg_indices, curr_idx, tmp_idx) {
    HASH_DEL(pkg_indices, curr_idx);
    free(curr_idx->key);
    free(curr_idx);
  }

  bool success = lockfile_writer_write(&writer, path);
  lockfile_writer_deinit(&writer);
  return success;
}
