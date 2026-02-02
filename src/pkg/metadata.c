#include "yyjson.h"
#include <stdlib.h>
#include <string.h>

static inline void copy_string_field(yyjson_mut_doc *doc, yyjson_mut_val *dest,
                                      yyjson_val *src, const char *key) {
  yyjson_val *val = yyjson_obj_get(src, key);
  if (val && yyjson_is_str(val)) {
    yyjson_mut_obj_add_strncpy(doc, dest, key,
                               yyjson_get_str(val), yyjson_get_len(val));
  }
}

static inline void copy_string_array(yyjson_mut_doc *doc, yyjson_mut_val *dest,
                                      yyjson_val *src, const char *key) {
  yyjson_val *arr = yyjson_obj_get(src, key);
  if (!arr || !yyjson_is_arr(arr)) return;

  yyjson_mut_val *out_arr = yyjson_mut_arr(doc);
  yyjson_val *elem;
  size_t idx, max;
  yyjson_arr_foreach(arr, idx, max, elem) {
    if (yyjson_is_str(elem)) {
      yyjson_mut_arr_add_strncpy(doc, out_arr,
                                 yyjson_get_str(elem), yyjson_get_len(elem));
    }
  }
  yyjson_mut_obj_add_val(doc, dest, key, out_arr);
}

static inline void copy_deps_object(yyjson_mut_doc *doc, yyjson_mut_val *dest,
                                     yyjson_val *src, const char *key) {
  yyjson_val *deps = yyjson_obj_get(src, key);
  if (!deps || !yyjson_is_obj(deps)) return;

  yyjson_mut_val *out_deps = yyjson_mut_obj(doc);
  yyjson_val *dep_key, *dep_val;
  size_t idx, max;
  yyjson_obj_foreach(deps, idx, max, dep_key, dep_val) {
    const char *k = yyjson_get_str(dep_key);
    size_t klen = yyjson_get_len(dep_key);

    if (yyjson_is_str(dep_val)) {
      yyjson_mut_val *mk = yyjson_mut_strncpy(doc, k, klen);
      yyjson_mut_val *mv = yyjson_mut_strncpy(doc, yyjson_get_str(dep_val),
                                              yyjson_get_len(dep_val));
      yyjson_mut_obj_add(out_deps, mk, mv);
    } else if (yyjson_is_obj(dep_val)) {
      yyjson_mut_val *nested = yyjson_mut_obj(doc);
      yyjson_mut_val *mk = yyjson_mut_strncpy(doc, k, klen);
      yyjson_mut_obj_add(out_deps, mk, nested);

      yyjson_val *nk, *nv;
      size_t ni, nm;
      yyjson_obj_foreach(dep_val, ni, nm, nk, nv) {
        const char *nks = yyjson_get_str(nk);
        size_t nkl = yyjson_get_len(nk);
        if (yyjson_is_str(nv)) {
          yyjson_mut_val *nmk = yyjson_mut_strncpy(doc, nks, nkl);
          yyjson_mut_val *nmv = yyjson_mut_strncpy(doc, yyjson_get_str(nv),
                                                   yyjson_get_len(nv));
          yyjson_mut_obj_add(nested, nmk, nmv);
        } else if (yyjson_is_bool(nv)) {
          yyjson_mut_val *nmk = yyjson_mut_strncpy(doc, nks, nkl);
          yyjson_mut_obj_add(nested, nmk, yyjson_mut_bool(doc, yyjson_get_bool(nv)));
        }
      }
    }
  }
  yyjson_mut_obj_add_val(doc, dest, key, out_deps);
}

static inline void copy_bin_field(yyjson_mut_doc *doc, yyjson_mut_val *dest,
                                   yyjson_val *src) {
  yyjson_val *bin = yyjson_obj_get(src, "bin");
  if (!bin) return;

  if (yyjson_is_str(bin)) {
    yyjson_mut_obj_add_strncpy(doc, dest, "bin",
                               yyjson_get_str(bin), yyjson_get_len(bin));
  } else if (yyjson_is_obj(bin)) {
    yyjson_mut_val *out_bin = yyjson_mut_obj(doc);
    yyjson_val *bk, *bv;
    size_t idx, max;
    yyjson_obj_foreach(bin, idx, max, bk, bv) {
      if (yyjson_is_str(bv)) {
        yyjson_mut_val *mk = yyjson_mut_strncpy(doc, yyjson_get_str(bk),
                                                yyjson_get_len(bk));
        yyjson_mut_val *mv = yyjson_mut_strncpy(doc, yyjson_get_str(bv),
                                                yyjson_get_len(bv));
        yyjson_mut_obj_add(out_bin, mk, mv);
      }
    }
    yyjson_mut_obj_add_val(doc, dest, "bin", out_bin);
  }
}

static inline void copy_dist(yyjson_mut_doc *doc, yyjson_mut_val *dest,
                              yyjson_val *src) {
  yyjson_val *dist = yyjson_obj_get(src, "dist");
  if (!dist || !yyjson_is_obj(dist)) return;

  yyjson_mut_val *out_dist = yyjson_mut_obj(doc);
  copy_string_field(doc, out_dist, dist, "tarball");
  copy_string_field(doc, out_dist, dist, "integrity");
  copy_string_field(doc, out_dist, dist, "shasum");
  yyjson_mut_obj_add_val(doc, dest, "dist", out_dist);
}

char *strip_npm_metadata(const char *json_data, size_t json_len, size_t *out_len) {
  yyjson_doc *doc = yyjson_read(json_data, json_len, 0);
  if (!doc) return NULL;

  yyjson_val *root = yyjson_doc_get_root(doc);
  if (!root || !yyjson_is_obj(root)) {
    yyjson_doc_free(doc);
    return NULL;
  }

  yyjson_mut_doc *mut_doc = yyjson_mut_doc_new(NULL);
  if (!mut_doc) {
    yyjson_doc_free(doc);
    return NULL;
  }

  yyjson_mut_val *out_root = yyjson_mut_obj(mut_doc);
  yyjson_mut_doc_set_root(mut_doc, out_root);

  copy_string_field(mut_doc, out_root, root, "name");

  yyjson_val *versions = yyjson_obj_get(root, "versions");
  if (versions && yyjson_is_obj(versions)) {
    yyjson_mut_val *out_versions = yyjson_mut_obj(mut_doc);

    yyjson_val *vkey, *vval;
    size_t idx, max;
    yyjson_obj_foreach(versions, idx, max, vkey, vval) {
      if (!yyjson_is_obj(vval)) continue;

      yyjson_mut_val *out_ver = yyjson_mut_obj(mut_doc);
      yyjson_mut_val *mk = yyjson_mut_strncpy(mut_doc, yyjson_get_str(vkey),
                                              yyjson_get_len(vkey));
      yyjson_mut_obj_add(out_versions, mk, out_ver);

      copy_string_field(mut_doc, out_ver, vval, "version");
      copy_deps_object(mut_doc, out_ver, vval, "dependencies");
      copy_deps_object(mut_doc, out_ver, vval, "peerDependencies");
      copy_deps_object(mut_doc, out_ver, vval, "optionalDependencies");
      copy_deps_object(mut_doc, out_ver, vval, "peerDependenciesMeta");
      copy_dist(mut_doc, out_ver, vval);
      copy_string_array(mut_doc, out_ver, vval, "os");
      copy_string_array(mut_doc, out_ver, vval, "cpu");
      copy_string_array(mut_doc, out_ver, vval, "libc");
      copy_bin_field(mut_doc, out_ver, vval);
    }
    yyjson_mut_obj_add_val(mut_doc, out_root, "versions", out_versions);
  }

  yyjson_val *dist_tags = yyjson_obj_get(root, "dist-tags");
  if (dist_tags && yyjson_is_obj(dist_tags)) {
    yyjson_val *latest = yyjson_obj_get(dist_tags, "latest");
    if (latest && yyjson_is_str(latest)) {
      yyjson_mut_val *out_tags = yyjson_mut_obj(mut_doc);
      yyjson_mut_obj_add_strncpy(mut_doc, out_tags, "latest",
                                 yyjson_get_str(latest), yyjson_get_len(latest));
      yyjson_mut_obj_add_val(mut_doc, out_root, "dist-tags", out_tags);
    }
  }

  char *result = yyjson_mut_write(mut_doc, 0, out_len);

  yyjson_mut_doc_free(mut_doc);
  yyjson_doc_free(doc);

  return result;
}

void strip_metadata_free(char *ptr) {
  free(ptr);
}
