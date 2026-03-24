#include <string.h>

#include "ant.h"
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
  { "SecurityError",              18 },
  { "NetworkError",               19 },
  { "AbortError",                 20 },
  { "URLMismatchError",           21 },
  { "QuotaExceededError",         22 },
  { "TimeoutError",               23 },
  { "InvalidNodeTypeError",       24 },
  { "DataCloneError",             25 },
};

static int name_to_code(const char *name) {
  if (!name) return 0;
  for (size_t i = 0; i < sizeof(domex_codes) / sizeof(domex_codes[0]); i++) {
    if (strcmp(name, domex_codes[i].name) == 0) return domex_codes[i].code;
  }
  return 0;
}

// new DOMException(message?, name?)
static ant_value_t domexception_ctor(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t self = js_getthis(js);

  const char *msg  = (nargs >= 1 && vtype(args[0]) == T_STR) ? js_getstr(js, args[0], NULL) : "";
  const char *name = (nargs >= 2 && vtype(args[1]) == T_STR) ? js_getstr(js, args[1], NULL) : "Error";

  int code = name_to_code(name);
  size_t msg_len  = strlen(msg);
  size_t name_len = strlen(name);

  js_set(js, self, "message", js_mkstr(js, msg,  msg_len));
  js_set(js, self, "name",    js_mkstr(js, name, name_len));
  js_set(js, self, "code",    js_mknum(code));

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

  return obj;
}

void init_domexception_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);

  ant_value_t proto = js_mkobj(js);
  g_domexception_proto = proto;
  g_initialized = true;

  js_set(js, proto, "name",    js_mkstr(js, "Error", 5));
  js_set(js, proto, "message", js_mkstr(js, "",      0));
  js_set(js, proto, "code",    js_mknum(0));
  js_set_sym(js, proto, get_toStringTag_sym(), js_mkstr(js, "DOMException", 12));

  ant_value_t ctor = js_mkobj(js);
  js_set_slot(ctor, SLOT_CFUNC, js_mkfun(domexception_ctor));
  js_mkprop_fast(js, ctor, "prototype", 9, proto);
  js_mkprop_fast(js, ctor, "name", 4, ANT_STRING("DOMException"));
  js_set_descriptor(js, ctor, "name", 4, 0);

  ant_value_t fn = js_obj_to_func(ctor);
  js_set(js, proto, "constructor", fn);
  js_set_descriptor(js, proto, "constructor", 11, JS_DESC_W | JS_DESC_C);

  js_set(js, global, "DOMException", fn);
}

void gc_mark_domexception(ant_t *js, gc_mark_fn mark) {
  if (g_initialized) mark(js, g_domexception_proto);
}
