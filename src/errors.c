#include "errors.h"
#include "error_render.h"
#include "internal.h"
#include "descriptors.h"
#include "output.h"
#include "modules/io.h"
#include "gc/roots.h"
#include "silver/stack_trace.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

void print_error_value(ant_t *js, ant_value_t value, ant_value_t fallback_stack, const char *prefix) {
  if (is_err(value)) {
    fallback_stack = Ant_Exception_Stack(js, value);
    value = Ant_Exception_Value(js, value);
  }

  ant_value_t obj = value;
  ant_output_stream_t *out = ant_output_stream(stderr);
  
  ant_value_t stack = js_mkundef();
  bool is_real_error = false, no_stack = false;
  ant_output_stream_begin(out);
  
  if (vtype(obj) == kTypeObject) {
    ant_value_t err_type = js_get_slot(obj, SLOT_ERR_TYPE);
    is_real_error = js_get_slot(obj, SLOT_ERROR_BRAND) == js_true || vtype(err_type) == kTypeNumber;
    no_stack = vtype(err_type) == kTypeNumber && ((int)js_getnum(err_type) & JS_ERR_NO_STACK);
    if (!no_stack) stack = js_getprop_fallback_len(js, obj, "stack", 5);
  }
  
  if (!no_stack && vtype(stack) != kTypeString) stack = fallback_stack;
  if (prefix) ant_output_stream_append_cstr(out, prefix);

  if (is_real_error && vtype(stack) == kTypeString) {
    io_print_error_stack(js, out, obj, stack);
  } else if (is_real_error) {
    io_print_error_header(js, out, obj);
    io_print_error_props(js, out, obj);
  } else ant_output_stream_append_cstr(out, js_str(js, value));

  ant_output_stream_putc(out, '\n');
  ant_output_stream_flush(out);
}

bool js_mark_errorlike_no_stack(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeObject) return false;
  if (js_get_slot(value, SLOT_ERROR_BRAND) == js_true) return false;

  const char *name = get_str_prop(js, value, "name", 4, NULL);
  const char *message = get_str_prop(js, value, "message", 7, NULL);
  if ((!name || !*name) && (!message || !*message)) return false;

  ant_value_t err_type = js_get_slot(value, SLOT_ERR_TYPE);
  int base_type = vtype(err_type) == kTypeNumber ? (int)js_getnum(err_type) : JS_ERR_GENERIC;

  js_set(js, value, "stack", js_mkundef());
  js_set_slot(value, SLOT_ERR_TYPE, js_mknum((double)(base_type | JS_ERR_NO_STACK)));
  
  return true;
}

static const char *get_error_type_name(js_err_type_t err_type) {
  static const char *names[] = {
    [JS_ERR_GENERIC]   = "Error",
    [JS_ERR_TYPE]      = "TypeError",
    [JS_ERR_SYNTAX]    = "SyntaxError",
    [JS_ERR_REFERENCE] = "ReferenceError",
    [JS_ERR_RANGE]     = "RangeError",
    [JS_ERR_EVAL]      = "EvalError",
    [JS_ERR_URI]       = "URIError",
    [JS_ERR_INTERNAL]  = "InternalError",
    [JS_ERR_AGGREGATE] = "AggregateError",
  };

  return names[err_type] ?: "Error";
}

static ant_value_t error_header_part(ant_t *js, ant_value_t obj, const char *key, size_t key_len, const char *fallback) {
  ant_value_t v = js_getprop_fallback_len(js, obj, key, key_len);
  if (is_err(v)) return v;
  if (vtype(v) == kTypeUndefined) return js_mkstr(js, fallback, strlen(fallback));
  return vtype(v) == kTypeString ? v : js_tostring_val(js, v);
}

ant_value_t js_error_header_parts(ant_t *js, ant_value_t obj, ant_value_t *name, ant_value_t *message) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  
  *name = error_header_part(js, obj, "name", 4, "Error");
  GC_ROOT_PIN(js, *name);
  *message = is_err(*name) ? *name : error_header_part(js, obj, "message", 7, "");
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(*message) ? *message : js_mkundef();
}

ant_value_t js_error_header_text(ant_t *js, ant_value_t obj) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t name, msg, failed = js_error_header_parts(js, obj, &name, &msg);
  
  GC_ROOT_PIN(js, name);
  GC_ROOT_PIN(js, msg);
  
  if (is_err(failed)) {
    GC_ROOT_RESTORE(js, root_mark);
    return failed;
  }

  size_t name_len = 0, msg_len = 0;
  const char *name_text = js_getstr(js, name, &name_len);
  const char *msg_text = js_getstr(js, msg, &msg_len);

  ant_value_t text = name_len == 0 ? msg : name;
  errbuf_t eb;
  
  if (name_len > 0 && msg_len > 0 && errbuf_init(&eb, name_len + msg_len + 3)) {
    size_t n = errbuf_appendf(&eb, 0, "%.*s: %.*s", (int)name_len, name_text, (int)msg_len, msg_text);
    text = errbuf_take_string(js, &eb, n);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return text;
}

ant_value_t js_error_stack_value(ant_t *js, ant_value_t obj) {
  ant_value_t text = js_mkundef(), rec = js_mkundef();
  
  for (int depth = 0; is_object_type(obj) && depth < 64; depth++) {
    text = js_get_slot(obj, SLOT_ERROR_STACK_TEXT);
    rec = js_get_slot(obj, SLOT_ERROR_STACK);
    if (js_is_error_record(rec) || vtype(text) != kTypeUndefined) break;
    obj = js_get_proto(js, obj);
  }

  if (!js_is_error_record(rec) || vtype(text) != kTypeUndefined) return text;

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, rec);
  
  js_set_slot(obj, SLOT_ERROR_STACK_TEXT, js_mkstr(js, "", 0));
  ant_value_t stack = js_error_header_text(js, obj);
  
  if (!is_err(stack)) stack = js_error_record_stack_text(js, stack, rec);
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, is_err(stack) ? js_mkundef() : stack);
  GC_ROOT_RESTORE(js, root_mark);
  
  return stack;
}

static ant_value_t builtin_error_stack_get(ant_params_t) {
  return js_error_stack_value(js, js_getthis(js));
}

static ant_value_t builtin_error_stack_set(ant_params_t) {
  ant_value_t obj = js_getthis(js);
  if (!is_object_type(obj)) return js_mkundef();
  js_set_slot(obj, SLOT_ERROR_STACK, js_mkundef());
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, nargs > 0 ? args[0] : js_mkundef());
  return js_mkundef();
}

void js_error_define_stack(ant_t *js, ant_value_t obj, ant_value_t record, ant_value_t text) {
  js_reserve_slots(obj, 3);
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK, record);
  if (vtype(text) != kTypeUndefined || vtype(js_get_slot(obj, SLOT_ERROR_STACK_TEXT)) != kTypeUndefined)
    js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, text);
  js_define_accessor_desc(
    js, js_as_obj(obj), "stack", 5,
    js_mkfun(builtin_error_stack_get),
    js_mkfun(builtin_error_stack_set), JS_DESC_C
  );
}

static bool error_has_captured_stack(ant_value_t obj) {
  return 
    js_is_error_record(js_get_slot(obj, SLOT_ERROR_STACK)) ||
    vtype(js_get_slot(obj, SLOT_ERROR_STACK_TEXT)) != kTypeUndefined;
}

static void error_capture_stack_at(ant_t *js, ant_value_t err_obj, const js_error_site_t *site) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, err_obj);
  js_error_define_stack(js, err_obj, js_error_record_capture(js, site), js_mkundef());
  GC_ROOT_RESTORE(js, root_mark);
}

void js_capture_stack(ant_t *js, ant_value_t err_obj) {
  error_capture_stack_at(js, err_obj, NULL);
}

static ant_value_t make_error_value(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type,
  ant_value_t props, const char *error_msg
) {
  bool no_stack = (err_type & JS_ERR_NO_STACK) != 0;
  js_err_type_t base_type = (js_err_type_t)(err_type & ~JS_ERR_NO_STACK);

  const char *err_name = get_error_type_name(base_type);
  size_t err_name_len = strlen(err_name);
  size_t msg_len = strlen(error_msg);

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, props);

  ant_value_t err_obj = js_mkobj(js);
  GC_ROOT_PIN(js, err_obj);

  if (is_err(err_obj)) {
    GC_ROOT_RESTORE(js, mark);
    return err_obj;
  }

  ant_value_t message = js_mkstr(js, error_msg, msg_len);
  GC_ROOT_PIN(js, message);
  mkprop_interned(js, err_obj, js->intern.message, message, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);
  js_set_slot(err_obj, SLOT_ERR_TYPE, js_mknum((double)err_type));

  int props_type = vtype(props);
  if ((T_FLAG_FIND(props_type) & T_SPECIAL_OBJECT_MASK) != 0)
    js_merge_obj(js, err_obj, props);

  ant_value_t proto = js_get_ctor_proto(js, err_name, err_name_len);
  int proto_type = vtype(proto);
  if ((T_FLAG_FIND(proto_type) & T_SPECIAL_OBJECT_MASK) != 0)
    js_set_proto_init(err_obj, proto);
  else {
    js_set(js, err_obj, "name", js_mkstr(js, err_name, err_name_len));
    js_set_descriptor(js, err_obj, "name", 4, JS_DESC_W | JS_DESC_C);
  }

  if (!no_stack) error_capture_stack_at(js, err_obj, site);
  GC_ROOT_RESTORE(js, mark);

  return err_obj;
}

__attribute__((format(printf, 5, 0)))
static ant_value_t CreateFormattedErrorValue(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type,
  ant_value_t props, const char *fmt, va_list args
) {
  char local[256];
  char *message = local;

  va_list copy;

  va_copy(copy, args);
  int length = vsnprintf(local, sizeof(local), fmt, copy);
  va_end(copy);

  if (length < 0) goto format_failed;

  if ((size_t)length >= sizeof(local)) {
    size_t capacity = (size_t)length + 1;
    message = malloc(capacity);

    if (!message) {
      ant_value_t failure = is_err(js->exception_oom) ? js->exception_oom : mkval(kTypeError, 0);
      Ant_Exception_Set(js, failure);
      return failure;
    }

    va_copy(copy, args);
    int written = vsnprintf(message, capacity, fmt, copy);
    va_end(copy);

    if (written < 0 || (size_t)written >= capacity) {
      free(message);
      goto format_failed;
    }
  }

  ant_value_t value = make_error_value(js, site, err_type, props, message);
  if (message != local) free(message);
  return value;

format_failed:
  return js_throw(js, Ant_Error_Create(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "failed to format error message"));
}

static ant_value_t raise_created_error(ant_t *js, ant_value_t value) {
  if (is_err(value)) return value;
  return Ant_Exception_Raise(js, value, js_mkundef());
}

__attribute__((format(printf, 4, 5)))
ant_value_t js_create_error(ant_t *js, js_err_type_t err_type, ant_value_t props, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, NULL, err_type, props, fmt, ap);
  va_end(ap);
  return raise_created_error(js, value);
}

__attribute__((format(printf, 5, 6)))
ant_value_t js_create_error_at(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type,
  ant_value_t props, const char *fmt, ...
) {
  va_list ap;
  va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, site, err_type, props, fmt, ap);
  va_end(ap);
  return raise_created_error(js, value);
}

ant_value_t Ant_Error_Create(ant_t *js, js_err_type_t err_type, const char *message) {
  return make_error_value(js, NULL, err_type, js_mkundef(), message);
}

__attribute__((format(printf, 3, 4)))
ant_value_t Ant_Error_CreateFormatted(ant_t *js, js_err_type_t err_type, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, NULL, err_type, js_mkundef(), fmt, ap);
  va_end(ap);
  return value;
}

ant_value_t js_throw(ant_t *js, ant_value_t value) {
  if (is_err(value)) {
    Ant_Exception_Set(js, value);
    return value;
  }

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, value);

  ant_value_t stack = js_mkundef();
  GC_ROOT_PIN(js, stack);

  bool no_stack = false;
  if (vtype(value) == kTypeObject) {
    ant_value_t kind = js_get_slot(value, SLOT_ERR_TYPE);
    if (error_has_captured_stack(value)) {
      no_stack = true;
      stack = js_get_slot(value, SLOT_ERROR_STACK_TEXT);
    } else if (vtype(kind) == kTypeNumber && ((int)js_getnum(kind) & JS_ERR_NO_STACK)) no_stack = true;
    else no_stack = js_try_get_own_data_prop(js, value, "stack", 5, &stack) && vtype(stack) == kTypeString;
    if (vtype(stack) != kTypeString) stack = js_mkundef();
  }

  if (!no_stack) stack = js_error_record_capture(js, NULL);
  ant_value_t result = Ant_Exception_Raise(js, value, stack);

  GC_ROOT_RESTORE(js, mark);
  return result;
}
