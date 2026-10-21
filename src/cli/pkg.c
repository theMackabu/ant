#include <compat.h> // IWYU pragma: keep

#include <stdbool.h>
#include <string.h>
#include <yyjson.h>
#include "cli/pkg.h"

bool pkg_verbose = false;
bool pkg_force = false;

bool pkg_script_exists(const char *package_json_path, const char *script_name) {
  if (!package_json_path || !script_name) return false;

  yyjson_doc *doc = yyjson_read_file(package_json_path, 0, NULL, NULL);
  if (!doc) return false;

  yyjson_val *scripts = yyjson_obj_get(yyjson_doc_get_root(doc), "scripts");
  yyjson_val *script = yyjson_obj_get(scripts, script_name);
  bool found = yyjson_is_str(script) && yyjson_get_len(script) > 0;

  yyjson_doc_free(doc);
  return found;
}
