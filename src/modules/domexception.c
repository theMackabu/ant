#include <string.h>

#include "ant.h"
#include "common.h"
#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "descriptors.h"

#include "gc/modules.h"
#include "modules/symbol.h"
#include "modules/domexception.h"

static ant_value_t g_domexception_proto = 0;
static bool g_initialized = false;

static const struct { const char *name; int code; } domex_codes[] = {
  { "IndexSizeError",             1  },
  { "HierarchyRequestError",      3  },
  { "WrongDocumentError",         4  },
  { "InvalidCharacterError",      5  },
  { "NoModificationAllowedError", 7  },
  { "NotFoundError",              8  },
  { "NotSupportedError",          9  },
  { "InUseAttributeError",        10 },
  { "InvalidStateError",          11 },
  { "SyntaxError",                12 },
  { "InvalidModificationError",   13 },
  { "NamespaceError",             14 },
  { "InvalidAccessError",         15 },
  { "TypeMismatchError",          17 },
  { "SecurityError",              18 },
  { "NetworkError",               19 },
  { "AbortError",                 20 },
  { "URLMismatchError",           21 },
  { "QuotaExceededError",         22 },
  { "TimeoutError",               23 },
  { "InvalidNodeTypeError",       24 },
  { "DataCloneError",             25 },
};

static const struct { const char *prop; int code; } domex_constants[] = {
  { "INDEX_SIZE_ERR",              1  },
  { "DOMSTRING_SIZE_ERR",          2  },
  { "HIERARCHY_REQUEST_ERR",       3  },
  { "WRONG_DOCUMENT_ERR",          4  },
  { "INVALID_CHARACTER_ERR",       5  },
  { "NO_DATA_ALLOWED_ERR",         6  },
  { "NO_MODIFICATION_ALLOWED_ERR", 7  },
  { "NOT_FOUND_ERR",               8  },
  { "NOT_SUPPORTED_ERR",           9  },
  { "INUSE_ATTRIBUTE_ERR",         10 },
  { "INVALID_STATE_ERR",           11 },
  { "SYNTAX_ERR",                  12 },
  { "INVALID_MODIFICATION_ERR",    13 },
  { "NAMESPACE_ERR",               14 },
  { "INVALID_ACCESS_ERR",          15 },
  { "VALIDATION_ERR",              16 },
  { "TYPE_MISMATCH_ERR",           17 },
  { "SECURITY_ERR",                18 },
  { "NETWORK_ERR",                 19 },
  { "ABORT_ERR",                   20 },
  { "URL_MISMATCH_ERR",            21 },
  { "QUOTA_EXCEEDED_ERR",          22 },
  { "TIMEOUT_ERR",                 23 },
  { "INVALID_NODE_TYPE_ERR",       24 },
  { "DATA_CLONE_ERR",              25 },
};

#define DOMEX_CONSTANTS_LEN (sizeof(domex_constants) / sizeof(domex_constants[0]))

static int name_to_code(const char *name) {
  if (!name) return 0;
  for (size_t i = 0; i < sizeof(domex_codes) / sizeof(domex_codes[0]); i++) {
    if (strcmp(name, domex_codes[i].name) == 0) return domex_codes[i].code;
  }
  return 0;
}

static void set_constants(ant_t *js, ant_value_t obj) {
for (size_t i = 0; i < DOMEX_CONSTANTS_LEN; i++) {
  const char *prop = domex_constants[i].prop;
  size_t len = strlen(prop);
  js_set(js, obj, prop, js_mknum(domex_constants[i].code));
  js_set_descriptor(js, obj, prop, len, JS_DESC_E);
}}

// new DOMException(message?, name?)
static ant_value_t domexception_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_getthis(js);

  const char *msg;
  if (nargs < 1 || vtype(args[0]) == T_UNDEF) {
    msg = "";
  } else if (vtype(args[0]) == T_STR) {
    msg = js_getstr(js, args[0], NULL);
    if (!msg) msg = "";
  } else {
    ant_value_t str_val = js_tostring_val(js, args[0]);
    msg = (vtype(str_val) == T_STR) ? js_getstr(js, str_val, NULL) : "";
    if (!msg) msg = "";
  }

  const char *name = (nargs >= 2 && vtype(args[1]) == T_STR) ? js_getstr(js, args[1], NULL) : "Error";
  if (!name) name = "Error";

  int code = name_to_code(name);
  size_t msg_len  = strlen(msg);
  size_t name_len = strlen(name);

  js_set(js, self, "message", js_mkstr(js, msg,  msg_len));
  js_set(js, self, "name",    js_mkstr(js, name, name_len));
  js_set(js, self, "code",    js_mknum(code));

  js_set_slot(self, SLOT_ERROR_BRAND, js_true);
  js_capture_stack(js, self);
  
  return js_mkundef();
}

ant_value_t make_dom_exception(ant_t *js, const char *message, const char *name) {
  size_t msg_len  = strlen(message);
  size_t name_len = strlen(name);
  int code = name_to_code(name);

  ant_value_t obj = js_mkobj(js);
  if (g_initialized) js_set_slot_wb(js, obj, SLOT_PROTO, g_domexception_proto);

  js_set(js, obj, "message", js_mkstr(js, message, msg_len));
  js_set(js, obj, "name",    js_mkstr(js, name,    name_len));
  js_set(js, obj, "code",    js_mknum(code));
  
  js_set_sym(js, obj, get_toStringTag_sym(), js_mkstr(js, "DOMException", 12));
  js_set_slot(obj, SLOT_ERROR_BRAND, js_true);

  return obj;
}

void init_domexception_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);

  ant_value_t proto = js_mkobj(js);
  g_domexception_proto = proto;
  g_initialized = true;

  ant_value_t error_proto = js_get_ctor_proto(js, "Error", 5);
  if (vtype(error_proto) != T_UNDEF) js_set_proto_init(proto, error_proto);

  js_set(js, proto, "name",    js_mkstr(js, "Error", 5));
  js_set(js, proto, "message", js_mkstr(js, "",      0));
  js_set(js, proto, "code",    js_mknum(0));
  
  js_set_sym(js, proto, get_toStringTag_sym(), js_mkstr(js, "DOMException", 12));
  set_constants(js, proto);

  ant_value_t ctor = js_mkobj(js);
  js_set_slot(ctor, SLOT_CFUNC, js_mkfun(domexception_ctor));
  js_mkprop_fast(js, ctor, "prototype", 9, proto);
  js_set_descriptor(js, ctor, "prototype", 9, 0);
  
  js_mkprop_fast(js, ctor, "name", 4, ANT_STRING("DOMException"));
  js_set_descriptor(js, ctor, "name", 4, 0);
  set_constants(js, ctor);

  ant_value_t fn = js_obj_to_func(ctor);
  js_set(js, proto, "constructor", fn);
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "DOMException", fn);
  js_set_descriptor(js, global, "DOMException", 12, JS_DESC_W | JS_DESC_C);
}

void gc_mark_domexception(ant_t *js, gc_mark_fn mark) {
  if (g_initialized) mark(js, g_domexception_proto);
}
