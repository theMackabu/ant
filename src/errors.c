#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "errors.h"
#include "utf8.h"
#include "cfunc.h"
#include "realm.h"
#include "error_render.h"
#include "internal.h"
#include "descriptors.h"
#include "output.h"
#include "gc/roots.h"
#include "silver/engine.h"
#include "silver/stack_trace.h"
#include "modules/io.h"
#include "modules/bigint.h"
#include "silver/call.h"
#include "modules/iterator.h"
#include "props.h"

void print_error_value(ant_t *js, ant_value_t value, ant_value_t fallback_stack, const char *prefix) {
  if (is_err(value)) {
    fallback_stack = Ant_Exception_Stack(js, value);
    value = Ant_Exception_Value(js, value);
  }

  ant_value_t obj = value;
  ant_output_stream_t *out = ant_output_stream(stderr);
  
  ant_value_t stack = Ant_Value_Undefined;
  bool is_real_error = false, no_stack = false;
  ant_output_stream_begin(out);
  
  if (vtype(obj) == kTypeObject) {
    ant_value_t err_type = js_get_slot(obj, SLOT_ERR_TYPE);
    is_real_error = js_get_slot(obj, SLOT_ERROR_BRAND) == Ant_Value_True || vtype(err_type) == kTypeNumber;
    no_stack = vtype(err_type) == kTypeNumber && ((int)Ant_Value_AsNumberUnchecked(err_type) & JS_ERR_NO_STACK);
    if (!no_stack) stack = js_getprop_fallback_len(js, obj, "stack", 5);
  }
  
  if (!no_stack && vtype(stack) != kTypeString) stack = fallback_stack;
  if (prefix) ant_output_stream_append_cstr(out, prefix);
  bool plain = !io_should_color_fd(fileno(stderr));

  if (is_real_error && vtype(stack) == kTypeString) {
    io_print_error_stack(js, out, obj, stack, plain);
  } else if (is_real_error) {
    io_print_error_header(js, out, obj, plain);
    io_print_error_props(js, out, obj, plain);
  } else ant_output_stream_append_cstr(out, js_str(js, value));

  ant_output_stream_putc(out, '\n');
  ant_output_stream_flush(out);
}

bool js_mark_errorlike_no_stack(ant_t *js, ant_value_t value) {
  if (vtype(value) != kTypeObject) return false;
  if (js_get_slot(value, SLOT_ERROR_BRAND) == Ant_Value_True) return false;

  const char *name = get_str_prop(js, value, "name", 4, NULL);
  const char *message = get_str_prop(js, value, "message", 7, NULL);
  if ((!name || !*name) && (!message || !*message)) return false;

  ant_value_t err_type = js_get_slot(value, SLOT_ERR_TYPE);
  int base_type = vtype(err_type) == kTypeNumber ? (int)Ant_Value_AsNumberUnchecked(err_type) : JS_ERR_GENERIC;

  js_set(js, value, "stack", Ant_Value_Undefined);
  js_set_slot(value, SLOT_ERR_TYPE, Ant_Value_FromNumber((double)(base_type | JS_ERR_NO_STACK)));
  
  return true;
}

static_assert(
  JS_ERR_TYPE_COUNT == sizeof(((ant_t *)0)->builtins.error_protos) / sizeof(ant_value_t),
  "resize error_protos in isolate_values.h"
);

const char *js_err_type_name(js_err_type_t err_type) {
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

static bool error_has_node_code(ant_value_t obj) {
  ant_value_t type = vtype(obj) == kTypeObject ? js_get_slot(obj, SLOT_ERR_TYPE) : Ant_Value_Undefined;
  return vtype(type) == kTypeNumber && ((int)Ant_Value_AsNumberUnchecked(type) & JS_ERR_NODE_CODE);
}

static bool error_props_have_node_code(ant_t *js, ant_value_t props) {
  if (!is_object_type(props)) return false;
  ant_value_t code = js_get(js, props, "code");
  size_t len = 0;
  const char *text = vtype(code) == kTypeString ? js_getstr(js, code, &len) : NULL;
  return text && len > 4 && memcmp(text, "ERR_", 4) == 0;
}

ant_value_t js_error_display_name(ant_t *js, ant_value_t obj, ant_value_t name) {
  if (!error_has_node_code(obj) || vtype(name) != kTypeString) return name;
  
  ant_value_t code = js_getprop_fallback_len(js, obj, "code", 4);
  if (is_err(code) || vtype(code) != kTypeString) return is_err(code) ? code : name;

  size_t name_len = 0, code_len = 0;
  const char *name_text = js_getstr(js, name, &name_len);
  const char *code_text = js_getstr(js, code, &code_len);
  
  errbuf_t eb;
  if (!errbuf_init(&eb, name_len + code_len + 3)) return name;
  size_t n = errbuf_append(&eb, 0, name_text, name_len);
  
  n = errbuf_append(&eb, n, " [", 2);
  n = errbuf_append(&eb, n, code_text, code_len);
  n = errbuf_append(&eb, n, "]", 1);
  
  return errbuf_take_string(js, &eb, n);
}

ant_value_t js_error_header_parts(ant_t *js, ant_value_t obj, ant_value_t *name, ant_value_t *message) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  
  *name = error_header_part(js, obj, "name", 4, "Error");
  GC_ROOT_PIN(js, *name);
  *message = is_err(*name) ? *name : error_header_part(js, obj, "message", 7, "");
  
  GC_ROOT_RESTORE(js, root_mark);
  return is_err(*message) ? *message : Ant_Value_Undefined;
}

ant_value_t js_error_header_text(ant_t *js, ant_value_t obj) {
  GC_ROOT_SAVE(root_mark, js);
  ant_value_t name, msg, failed = js_error_header_parts(js, obj, &name, &msg);
  
  if (!is_err(failed)) name = js_error_display_name(js, obj, name);
  if (is_err(name)) failed = name;
  
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
    size_t n = errbuf_append(&eb, 0, name_text, name_len);
    n = errbuf_append(&eb, n, ": ", 2);
    n = errbuf_append(&eb, n, msg_text, msg_len);
    text = errbuf_take_string(js, &eb, n);
  }

  GC_ROOT_RESTORE(js, root_mark);
  return text;
}

ant_value_t js_error_stack_value(ant_t *js, ant_value_t obj) {
  ant_value_t text = Ant_Value_Undefined, rec = Ant_Value_Undefined;
  
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
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, is_err(stack) ? Ant_Value_Undefined : stack);
  GC_ROOT_RESTORE(js, root_mark);
  
  return stack;
}

static ant_value_t builtin_error_stack_get(ant_params_t) {
  return js_error_stack_value(js, call_this);
}

static ant_value_t builtin_error_stack_set(ant_params_t) {
  if (!is_object_type(call_this)) return Ant_Value_Undefined;
  js_set_slot(call_this, SLOT_ERROR_STACK, Ant_Value_Undefined);
  js_set_slot_wb(js, call_this, SLOT_ERROR_STACK_TEXT, nargs > 0 ? args[0] : Ant_Value_Undefined);
  return Ant_Value_Undefined;
}

void js_error_define_stack(ant_t *js, ant_value_t obj, ant_value_t record, ant_value_t text) {
  js_reserve_slots(obj, 3);
  js_set_slot_wb(js, obj, SLOT_ERROR_STACK, record);
  
  if (
    vtype(text) != kTypeUndefined || 
    vtype(js_get_slot(obj, SLOT_ERROR_STACK_TEXT)) != kTypeUndefined
  ) js_set_slot_wb(js, obj, SLOT_ERROR_STACK_TEXT, text);
    
  js_define_accessor_desc(
    js, js_as_obj(obj), "stack", 5,
    Ant_Function_CreateWithMeta(builtin_error_stack_get, "get stack", 0, 0),
    Ant_Function_CreateWithMeta(builtin_error_stack_set, "set stack", 1, 0), JS_DESC_C
  );
}

static bool error_has_captured_stack(ant_value_t obj) {
  return 
    js_is_error_record(js_get_slot(obj, SLOT_ERROR_STACK)) ||
    vtype(js_get_slot(obj, SLOT_ERROR_STACK_TEXT)) != kTypeUndefined;
}

static void error_capture_stack_at(ant_t *js, ant_value_t call_func, ant_value_t err_obj, const js_error_site_t *site, ant_value_t above) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, err_obj);
  js_error_define_stack(js, err_obj, js_error_record_capture(js, call_func, site, above), Ant_Value_Undefined);
  GC_ROOT_RESTORE(js, root_mark);
}

void js_capture_stack(ant_t *js, ant_value_t err_obj) {
  error_capture_stack_at(js, Ant_Value_Undefined, err_obj, NULL, Ant_Value_Undefined);
}

void js_capture_stack_above(ant_t *js, ant_value_t call_func, ant_value_t err_obj, ant_value_t above) {
  error_capture_stack_at(js, call_func, err_obj, NULL, above);
}

static ant_value_t make_error_value(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type,
  ant_value_t props, const char *error_msg
) {
  bool no_stack = (err_type & JS_ERR_NO_STACK) != 0;
  js_err_type_t base_type = (js_err_type_t)(err_type & ~JS_ERR_NO_STACK);

  const char *err_name = js_err_type_name(base_type);
  size_t err_name_len = strlen(err_name);
  size_t msg_len = strlen(error_msg);

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, props);

  ant_value_t err_obj = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, err_obj);

  if (is_err(err_obj)) {
    GC_ROOT_RESTORE(js, mark);
    return err_obj;
  }

  ant_value_t message = js_mkstr(js, error_msg, msg_len);
  GC_ROOT_PIN(js, message);
  mkprop_interned(js, err_obj, js->intern.message, message, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);

  int type_bits = err_type;
  int props_type = vtype(props);
  if ((T_FLAG_FIND(props_type) & T_SPECIAL_OBJECT_MASK) != 0) {
    js_merge_obj(js, err_obj, props);
    if (error_props_have_node_code(js, props)) type_bits |= JS_ERR_NODE_CODE;
  }
  
  js_set_slot(err_obj, SLOT_ERR_TYPE, Ant_Value_FromNumber((double)type_bits));
  js_set_slot(err_obj, SLOT_ERROR_BRAND, Ant_Value_True);
  
  ant_value_t proto = js->error_realm
    ? js->error_realm->builtins.error_protos[base_type]
    : js->builtins.error_protos[base_type];
  
  int proto_type = vtype(proto);
  if ((T_FLAG_FIND(proto_type) & T_SPECIAL_OBJECT_MASK) != 0)
    js_set_proto_init(err_obj, proto);
  else {
    js_set(js, err_obj, "name", js_mkstr(js, err_name, err_name_len));
    js_set_descriptor(js, err_obj, "name", 4, JS_DESC_W | JS_DESC_C);
  }

  if (!no_stack) error_capture_stack_at(js, Ant_Value_Undefined, err_obj, site, Ant_Value_Undefined);
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

  char *clean = wtf8_replace_invalid(message, (size_t)length, NULL);
  ant_value_t value = make_error_value(js, site, err_type, props, clean ? clean : message);
  
  free(clean);
  if (message != local) free(message);
  
  return value;

format_failed:
  return js_throw(js, Ant_Error_Create(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "failed to format error message"));
}

static ant_value_t raise_created_error(ant_t *js, ant_value_t value) {
  if (is_err(value)) return value;
  return Ant_Exception_Raise(js, value, Ant_Value_Undefined);
}

__attribute__((format(printf, 4, 5))) ant_value_t js_create_error(
  ant_t *js, js_err_type_t err_type, ant_value_t props, const char *fmt, ...
) {
  if (__builtin_expect(js->quiet_errors, 0)) return js_throw(js, mkval(kTypeError, 0));
  
  va_list ap; va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, NULL, err_type, props, fmt, ap);
  va_end(ap);
  
  return raise_created_error(js, value);
}

__attribute__((format(printf, 5, 6))) ant_value_t js_create_error_at(
  ant_t *js, const js_error_site_t *site, js_err_type_t err_type, ant_value_t props, const char *fmt, ...
) {
  if (__builtin_expect(js->quiet_errors, 0)) return js_throw(js, mkval(kTypeError, 0));
  
  va_list ap; va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, site, err_type, props, fmt, ap);
  va_end(ap);
  
  return raise_created_error(js, value);
}

ant_value_t Ant_Error_Create(ant_t *js, js_err_type_t err_type, const char *message) {
  return make_error_value(js, NULL, err_type, Ant_Value_Undefined, message);
}

ant_value_t Ant_Error_CreateWithProps(ant_t *js, js_err_type_t err_type, ant_value_t props, const char *message) {
  return make_error_value(js, NULL, err_type, props, message);
}

__attribute__((format(printf, 3, 4)))
ant_value_t Ant_Error_CreateFormatted(ant_t *js, js_err_type_t err_type, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  ant_value_t value = CreateFormattedErrorValue(js, NULL, err_type, Ant_Value_Undefined, fmt, ap);
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

  ant_value_t stack = Ant_Value_Undefined;
  GC_ROOT_PIN(js, stack);

  bool no_stack = false;
  if (vtype(value) == kTypeObject) {
    ant_value_t kind = js_get_slot(value, SLOT_ERR_TYPE);
    if (error_has_captured_stack(value)) {
      no_stack = true;
      stack = js_get_slot(value, SLOT_ERROR_STACK_TEXT);
    } else if (vtype(kind) == kTypeNumber && ((int)Ant_Value_AsNumberUnchecked(kind) & JS_ERR_NO_STACK)) no_stack = true;
    else no_stack = js_try_get_own_data_prop(js, value, "stack", 5, &stack) && vtype(stack) == kTypeString;
    if (vtype(stack) != kTypeString) stack = Ant_Value_Undefined;
  }

  if (!no_stack) stack = js_error_record_capture(js, Ant_Value_Undefined, NULL, Ant_Value_Undefined);
  ant_value_t result = Ant_Exception_Raise(js, value, stack);

  GC_ROOT_RESTORE(js, mark);
  return result;
}

__attribute__((noinline, cold)) ant_value_t js_mkerr_code(ant_t *js, js_err_type_t type, const char *code, const char *message) {
  ant_value_t props = Ant_Object_CreateBare(js);
  js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  return js_mkerr_props(js, type, props, "%s", message);
}

void js_describe_received(ant_t *js, ant_value_t value, char *out, size_t out_len) {
  ant_value_type_t type = vtype(value);

  if (type == kTypeUndefined || type == kTypeNull) {
    snprintf(out, out_len, "Received %s", type == kTypeNull ? "null" : "undefined");
    return;
  }

  if (is_object_type(value)) {
    bool callable = is_callable(value);
    
    ant_value_t named = callable ? value : js_getprop_fallback(js, value, "constructor");
    ant_value_t name = is_err(named) || !is_object_type(named) ? Ant_Value_Undefined : js_getprop_fallback(js, named, "name");
    if (is_err(named) || is_err(name)) Ant_Exception_Clear(js);

    size_t name_len = 0;
    const char *name_str = vtype(name) == kTypeString ? js_getstr(js, name, &name_len) : NULL;
    
    if (callable) snprintf(out, out_len, "Received function %.*s", (int)name_len, name_str ? name_str : "");
    else if (name_len) snprintf(out, out_len, "Received an instance of %.*s", (int)name_len, name_str);
    else snprintf(out, out_len, "Received type object");
    
    return;
  }

  char buf[64];
  js_cstr_t received = js_to_cstr(js, value, buf, sizeof(buf));
  const char *quote = type == kTypeString ? "'" : "";
  
  snprintf(
    out, out_len, "Received type %s (%s%.25s%s%s)",
    typestr(type), quote, received.ptr, received.len > 28 ? "..." : "", quote
  );
  
  if (received.needs_free) free((void *)received.ptr);
}

static const char *js_arg_kind(const char *name) {
  return strchr(name, '.') ? "property" : "argument";
}

__attribute__((noinline, cold)) ant_value_t Ant_Error_CreateInvalidArgType(
  ant_t *js, const char *name, const char *expected, ant_value_t received
) {
  char got[160], message[320];
  js_describe_received(js, received, got, sizeof(got));
  snprintf(message, sizeof(message), "The \"%s\" %s must be %s. %s", name, js_arg_kind(name), expected, got);
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", message);
}

__attribute__((noinline, cold)) ant_value_t Ant_Error_CreateInvalidArgValue(
  ant_t *js, const char *name, const char *expected, ant_value_t received
) {
  char inspected[160], message[384];
  js_cstr_t text = js_inspect_cstr(js, received, inspected, sizeof(inspected));

  if (expected) snprintf(
    message, sizeof(message), "The %s '%s' must be one of: %s. Received %s",
    js_arg_kind(name), name, expected, text.ptr
  );

  else snprintf(message, sizeof(message), "The %s '%s' is invalid. Received %s", js_arg_kind(name), name, text.ptr);
  if (text.needs_free) free((void *)text.ptr);

  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_VALUE", message);
}

ant_value_t Ant_Error_CheckNoNullBytes(ant_t *js, const char *name, ant_value_t value) {
  size_t len = 0;
  const char *str = vtype(value) == kTypeString ? js_getstr(js, value, &len) : NULL;
  if (!str || !memchr(str, '\0', len)) return Ant_Value_Undefined;

  char inspected[160], message[384];
  js_cstr_t text = js_inspect_cstr(js, value, inspected, sizeof(inspected));
  snprintf(
    message, sizeof(message), "The %s '%s' must be a string without null bytes. Received %s",
    js_arg_kind(name), name, text.ptr
  );
  
  if (text.needs_free) free((void *)text.ptr);
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_VALUE", message);
}

static void js_group_digits(const char *digits, char *out, size_t len) {
  size_t n = strlen(digits), start = digits[0] == '-' ? 1 : 0, head = n;
  while (head >= start + 4) head -= 3;
  
  size_t w = 0;
  for (size_t i = 0; i < n && w + 1 < len; i++) {
    if (i >= head && i > start && (i - head) % 3 == 0) out[w++] = '_';
    if (w + 1 < len) out[w++] = digits[i];
  }
  out[w] = '\0';
}

static void js_describe_range_value(ant_t *js, ant_value_t value, char *out, size_t len) {
  char raw[96];
  
  if (vtype(value) == kTypeBigInt) {
    size_t n = strbigint(js, value, raw, sizeof(raw) - 1);
    raw[n < sizeof(raw) - 1 ? n : sizeof(raw) - 2] = '\0';
    const char *digits = raw[0] == '-' ? raw + 1 : raw;
    bool large = strlen(digits) > 10 || (strlen(digits) == 10 && strcmp(digits, "4294967296") > 0);
    if (large) js_group_digits(raw, out, len - 1);
    else snprintf(out, len - 1, "%s", raw);
    strncat(out, "n", len - strlen(out) - 1);
    return;
  }
  
  double d = Ant_Value_AsNumberUnchecked(value);
  if (d == 0 && signbit(d)) { snprintf(out, len, "-0"); return; }
  
  size_t n = js_number_to_chars(value, raw, sizeof(raw) - 1);
  raw[n < sizeof(raw) - 1 ? n : sizeof(raw) - 2] = '\0';
  if (isfinite(d) && d == trunc(d) && fabs(d) > 4294967296.0) js_group_digits(raw, out, len);
  else snprintf(out, len, "%s", raw);
}

__attribute__((noinline, cold)) ant_value_t Ant_Error_CreateStackOverflow(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_RANGE | JS_ERR_NO_STACK, "Maximum call stack size exceeded");
}

__attribute__((noinline, cold)) ant_value_t Ant_Error_CreateOutOfMemory(ant_t *js) {
  return js_mkerr_typed(js, JS_ERR_INTERNAL | JS_ERR_NO_STACK, "Memory allocation failed");
}

__attribute__((noinline, cold)) ant_value_t Ant_Error_CreateOutOfRange(
  ant_t *js, const char *name, const char *range, ant_value_t received
) {
  char got[128], message[384];
  js_describe_range_value(js, received, got, sizeof(got));
  snprintf(message, sizeof(message), "The value of \"%s\" is out of range. It must be %s. Received %s", name, range, got);
  return js_mkerr_code(js, JS_ERR_RANGE, "ERR_OUT_OF_RANGE", message);
}

static ant_value_t builtin_error_captureStackTrace(ant_params_t) {
  if (nargs < 1 || !is_object_type(args[0]))
    return js_mkerr(js, "argument must be an object");

  ant_value_t target = args[0];
  ant_value_t above = nargs > 1 ? args[1] : Ant_Value_Undefined;
  
  if (vtype(above) == kTypeBuiltin && js_as_cfunc(above) == builtin_error_captureStackTrace) above = Ant_Value_Undefined;
  ant_value_t error_ctor = lkp_val(js, js_glob(js), "Error", 5);
  ant_value_t prep = Ant_Value_Undefined;

  if (vtype(error_ctor) == kTypeFunction || vtype(error_ctor) == kTypeBuiltin)
    prep = lkp_val(js, js_func_obj(error_ctor), "prepareStackTrace", 17);

  if (vtype(prep) == kTypeFunction || vtype(prep) == kTypeBuiltin) {
    ant_value_t callsites = js_build_callsite_array(js, call_func, above);
    ant_value_t prep_args[2] = { target, callsites };
    
    ant_value_t result = sv_vm_call(js->vm, js, prep, Ant_Value_Undefined, prep_args, 2, NULL, Ant_Value_Undefined);
    if (Ant_Exception_Pending(js)) return Ant_Value_Undefined;
    
    js_error_define_stack(js, target, Ant_Value_Undefined, result);
  } else js_capture_stack_above(js, call_func, target, above);

  return Ant_Value_Undefined;
}

static ant_value_t builtin_error_isError(ant_params_t) {
  if (nargs < 1) return Ant_Value_False;
  ant_value_t val = args[0];
  if (!is_object_type(val)) return Ant_Value_False;
  return js_get_slot(val, SLOT_ERROR_BRAND) == Ant_Value_True ? Ant_Value_True : Ant_Value_False;
}

static ant_value_t error_install_message_cause(ant_t *js, ant_value_t err, ant_value_t message, ant_value_t options) {
  if (vtype(message) != kTypeUndefined) {
    ant_value_t msg = js_template_to_string(js, message);
    if (is_err(msg)) return msg;
    mkprop_interned(js, err, js->intern.message, msg, ANT_PROP_ATTR_WRITABLE | ANT_PROP_ATTR_CONFIGURABLE);
  }
  
  if (is_object_type(options) && lkp_proto(js, options, "cause", 5).obj) {
    ant_value_t cause = js_get(js, options, "cause");
    if (is_err(cause)) return cause;
    mkprop_bytes(js, err, "cause", 5, cause, JS_DESC_W | JS_DESC_C);
  }
  
  return Ant_Value_Undefined;
}

static ant_value_t builtin_Error(ant_params_t) {
  bool is_new = (vtype(call_new_target) != kTypeUndefined);
  ant_value_t this_val = call_this;
  
  if (!is_new) {
    this_val = Ant_Object_CreateBare(js);
    ant_value_t proto = lkp_interned_val(js, js_func_obj(call_func), js->intern.prototype);
    if (vtype(proto) != kTypeUndefined) js_set_proto_init(this_val, proto);
    else js_set_proto_init(this_val, js->builtins.error_protos[JS_ERR_GENERIC]);
  }
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, this_val);
  js_capture_stack_above(js, call_func, this_val, call_new_target);
  
  ant_value_t installed = error_install_message_cause(
    js, this_val, nargs > 0 ? args[0] : Ant_Value_Undefined, 
    nargs > 1 ? args[1] : Ant_Value_Undefined
  );
  
  js_set_slot(this_val, SLOT_ERROR_BRAND, Ant_Value_True);
  GC_ROOT_RESTORE(js, mark);
  
  return is_err(installed) ? installed : this_val;
}

ant_value_t Ant_Error_CreateConstructorEx(ant_t *js, const char *name, ant_value_t cfunc, ant_value_t *out_proto) {
  size_t name_len = strlen(name);
  ant_value_t error_ctor = js->builtins.error_ctor;
  
  ant_value_t proto = Ant_Object_CreateBare(js);
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, proto);
  
  set_proto(js, proto, js->builtins.error_protos[JS_ERR_GENERIC]);
  mkprop_bytes(js, proto, "name", 4, js_mkstr(js, name, name_len), JS_DESC_W | JS_DESC_C);
  mkprop_bytes(js, proto, "message", 7, js_mkstr(js, "", 0), JS_DESC_W | JS_DESC_C);
  
  ant_value_t ctor = mkobj(js, 0);
  GC_ROOT_PIN(js, ctor);
  
  set_proto(js, ctor, error_ctor);
  js_set_slot(ctor, SLOT_CFUNC, cfunc);
  js_setprop_readonly_nonconfigurable(js, ctor, "prototype", 9, proto);
  js_set_function_name(js, ctor, name, name_len);
  
  ant_value_t ctor_func = js_obj_to_func(js, ctor);
  defhidden(js, proto, "constructor", 11, ctor_func);
  GC_ROOT_RESTORE(js, mark);
  
  if (out_proto) *out_proto = proto;
  return ctor_func;
}

ant_value_t Ant_Error_CreateConstructor(ant_t *js, const char *name, ant_value_t *out_proto) {
  return Ant_Error_CreateConstructorEx(js, name, Ant_Function_CreateWithArity(builtin_Error, 1), out_proto);
}

static ant_value_t builtin_Error_toString(ant_params_t) {  
  ant_value_t name = js_getprop_fallback_len(js, call_this, "name", 4);
  if (vtype(name) == kTypeUndefined) name = js_mkstr(js, "Error", 5);
  else if (vtype(name) != kTypeString) {
    const char *s = js_str(js, name);
    name = js_mkstr(js, s, strlen(s));
  }
  name = js_error_display_name(js, call_this, name);
  if (is_err(name)) return name;
  
  ant_value_t msg = js_getprop_fallback_len(js, call_this, "message", 7);
  if (vtype(msg) == kTypeUndefined) msg = js_mkstr(js, "", 0);
  else if (vtype(msg) != kTypeString) {
    const char *s = js_str(js, msg);
    msg = js_mkstr(js, s, strlen(s));
  }
  
  ant_offset_t name_len, msg_len;
  ant_offset_t name_off = vstr(js, name, &name_len);
  ant_offset_t msg_off = vstr(js, msg, &msg_len);
  
  const char *name_str = (const char *)(uintptr_t)(name_off);
  const char *msg_str = (const char *)(uintptr_t)(msg_off);
  
  if (name_len == 0) return msg;
  if (msg_len == 0) return name;

  size_t total = (size_t)(name_len + 2 + msg_len);
  char *buf = malloc(total + 1);
  if (!buf) return Ant_Error_CreateOutOfMemory(js);

  memcpy(buf, name_str, (size_t)name_len);
  buf[name_len] = ':';
  buf[name_len + 1] = ' ';
  memcpy(buf + name_len + 2, msg_str, (size_t)msg_len);
  buf[total] = '\0';
  
  ant_value_t result = js_mkstr(js, buf, total);
  free(buf);
  
  return result;
}

static ant_value_t builtin_AggregateError(ant_params_t) {
  bool is_new = (vtype(call_new_target) != kTypeUndefined);
  ant_value_t this_val = call_this;
  
  if (!is_new) {
    this_val = Ant_Object_CreateBare(js);
    ant_prop_loc_t proto_off = lkp_interned(js_func_obj(call_func), js->intern.prototype);
    if (proto_off.obj) js_set_proto_init(this_val, js_prop_load(proto_off));
    else js_set_proto_init(this_val, js->builtins.error_protos[JS_ERR_AGGREGATE]);
  }
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, this_val);
  
  ant_value_t installed = error_install_message_cause(
    js, this_val, nargs > 1 ? args[1] : Ant_Value_Undefined, 
    nargs > 2 ? args[2] : Ant_Value_Undefined
  );
  
  if (is_err(installed)) { 
    GC_ROOT_RESTORE(js, mark); 
    return installed;
  }
  
  ant_value_t errors = Ant_Iterator_ToList(js, nargs > 0 ? args[0] : Ant_Value_Undefined, "AggregateError errors is not iterable");
  if (is_err(errors)) {
    GC_ROOT_RESTORE(js, mark);
    return errors;
  }
  
  mkprop_bytes(js, this_val, "errors", 6, errors, JS_DESC_W | JS_DESC_C);
  js_set_slot(this_val, SLOT_ERROR_BRAND, Ant_Value_True);
  
  js_capture_stack_above(js, call_func, this_val, call_new_target);
  GC_ROOT_RESTORE(js, mark);

  return this_val;
}

static ant_value_t builtin_SuppressedError(ant_params_t) {
  bool is_new = (vtype(call_new_target) != kTypeUndefined);
  ant_value_t this_val = call_this;

  if (!is_new) {
    this_val = Ant_Object_CreateBare(js);
    ant_prop_loc_t proto_off = lkp_interned(js_func_obj(call_func), js->intern.prototype);
    if (proto_off.obj) js_set_proto_init(this_val, js_prop_load(proto_off));
    else js_set_proto_init(this_val, js->builtins.suppressed_error_proto);
  }

  ant_value_t error = nargs > 0 ? args[0] : Ant_Value_Undefined;
  ant_value_t suppressed = nargs > 1 ? args[1] : Ant_Value_Undefined;
  
  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, this_val);
  
  ant_value_t installed = error_install_message_cause(js, this_val, nargs > 2 ? args[2] : Ant_Value_Undefined, Ant_Value_Undefined);
  
  GC_ROOT_RESTORE(js, mark);
  if (is_err(installed)) return installed;

  mkprop_bytes(js, this_val, "error", 5, error, JS_DESC_W | JS_DESC_C);
  mkprop_bytes(js, this_val, "suppressed", 10, suppressed, JS_DESC_W | JS_DESC_C);

  js_set_slot(this_val, SLOT_ERROR_BRAND, Ant_Value_True);
  js_capture_stack_above(js, call_func, this_val, call_new_target);

  return this_val;
}

void js_errors_init(ant_t *js, ant_value_t object_proto, ant_value_t function_proto) {
  ant_value_t error_proto = Ant_Object_CreateBare(js);
  set_proto(js, error_proto, object_proto);
  
  mkprop_bytes(js, error_proto, "name", 4, Ant_String_FromLiteral(js, "Error"), JS_DESC_W | JS_DESC_C);
  mkprop_bytes(js, error_proto, "message", 7, js_mkstr(js, "", 0), JS_DESC_W | JS_DESC_C);
  defmethod(js, error_proto, "toString", 8, Ant_Function_Create(builtin_Error_toString));
  js->builtins.error_protos[JS_ERR_GENERIC] = error_proto;
  
  ant_value_t err_ctor_obj = mkobj(js, 0);
  set_proto(js, err_ctor_obj, function_proto);
  js_set_slot(err_ctor_obj, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_Error, 1));
  js_setprop_readonly_nonconfigurable(js, err_ctor_obj, "prototype", 9, error_proto);
  js_set_function_name(js, err_ctor_obj, "Error", sizeof("Error") - 1);
  
  ant_value_t err_ctor_func = js_obj_to_func(js, err_ctor_obj);
  js_set_global_builtin(js, "Error", err_ctor_func);
  js->builtins.error_ctor = err_ctor_func;
  defhidden(js, error_proto, "constructor", 11, err_ctor_func);
  defmethod(js, err_ctor_func, "isError", 7, Ant_Function_CreateWithArity(builtin_error_isError, 1));
  defmethod(js, err_ctor_func, "captureStackTrace", 17, Ant_Function_CreateWithArity(builtin_error_captureStackTrace, 2));
  js_setprop(js, err_ctor_func, Ant_String_FromLiteral(js, "stackTraceLimit"), Ant_Value_FromNumber(10));
  
  #define REGISTER_ERROR_SUBTYPE(err_type, name_str) do { \
    ant_value_t ctor_func = Ant_Error_CreateConstructor(js, name_str, &js->builtins.error_protos[err_type]); \
    js_set_global_builtin(js, name_str, ctor_func); \
  } while(0)
  
  REGISTER_ERROR_SUBTYPE(JS_ERR_EVAL, "EvalError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_RANGE, "RangeError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_REFERENCE, "ReferenceError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_SYNTAX, "SyntaxError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_TYPE, "TypeError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_URI, "URIError");
  REGISTER_ERROR_SUBTYPE(JS_ERR_INTERNAL, "InternalError");
  
  #undef REGISTER_ERROR_SUBTYPE
  
  ant_value_t aggregate_proto = Ant_Object_CreateBare(js);
  set_proto(js, aggregate_proto, error_proto);
  mkprop_bytes(js, aggregate_proto, "name", 4, Ant_String_FromLiteral(js, "AggregateError"), JS_DESC_W | JS_DESC_C);
  js->builtins.error_protos[JS_ERR_AGGREGATE] = aggregate_proto;
  
  ant_value_t aggregate_ctor = mkobj(js, 0);
  set_proto(js, aggregate_ctor, err_ctor_func);
  js_set_slot(aggregate_ctor, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_AggregateError, 2));
  js_setprop_readonly_nonconfigurable(js, aggregate_ctor, "prototype", 9, aggregate_proto);
  js_set_function_name(js, aggregate_ctor, "AggregateError", sizeof("AggregateError") - 1);
  
  ant_value_t aggregate_ctor_func = js_obj_to_func(js, aggregate_ctor);
  defhidden(js, aggregate_proto, "constructor", 11, aggregate_ctor_func);
  js_set_global_builtin(js, "AggregateError", aggregate_ctor_func);

  ant_value_t suppressed_proto = Ant_Object_CreateBare(js);
  set_proto(js, suppressed_proto, error_proto);
  mkprop_bytes(js, suppressed_proto, "name", 4, Ant_String_FromLiteral(js, "SuppressedError"), JS_DESC_W | JS_DESC_C);
  js->builtins.suppressed_error_proto = suppressed_proto;
  
  ant_value_t suppressed_ctor = mkobj(js, 0);
  set_proto(js, suppressed_ctor, err_ctor_func);
  js_set_slot(suppressed_ctor, SLOT_CFUNC, Ant_Function_CreateWithArity(builtin_SuppressedError, 3));
  js_setprop_readonly_nonconfigurable(js, suppressed_ctor, "prototype", 9, suppressed_proto);
  js_set_function_name(js, suppressed_ctor, "SuppressedError", sizeof("SuppressedError") - 1);
  
  ant_value_t suppressed_ctor_func = js_obj_to_func(js, suppressed_ctor);
  defhidden(js, suppressed_proto, "constructor", 11, suppressed_ctor_func);
  js_set_global_builtin(js, "SuppressedError", suppressed_ctor_func);
}

typedef enum {
  PROPERTY_ERROR_READ_ONLY,
  PROPERTY_ERROR_GETTER_ONLY,
  PROPERTY_ERROR_PRIMITIVE,
} property_error_kind_t;

static const char *property_error_receiver_kind(ant_value_t receiver) {
  if (is_callable(receiver)) return "function";
  switch (vtype(receiver)) {
    case kTypeString: return "string";
    case kTypeNumber: return "number";
    case kTypeBool: return "boolean";
    case kTypeSymbol: return "symbol";
    case kTypeBigInt: return "bigint";
    default: return "object";
  }
}

static ant_value_t property_error_key_text(ant_t *js, ant_value_t key) {
  if (vtype(key) == kTypeSymbol) return js_symbol_to_string(js, key);
  if (vtype(key) == kTypeString) return key;
  return js_tostring_val(js, key);
}

static __attribute__((noinline, cold)) ant_value_t property_error(
  ant_t *js, property_error_kind_t kind, ant_value_t receiver, ant_value_t key
) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, receiver);
  GC_ROOT_PIN(js, key);

  ant_value_t key_text = property_error_key_text(js, key);
  GC_ROOT_PIN(js, key_text);
  ant_value_t described = is_err(key_text) ? key_text : Ant_Value_Describe(js, receiver);
  GC_ROOT_PIN(js, described);

  if (is_err(described)) {
    GC_ROOT_RESTORE(js, root_mark);
    return described;
  }

  size_t key_len = 0, described_len = 0;
  const char *key_bytes = js_getstr(js, key_text, &key_len);
  const char *described_bytes = js_getstr(js, described, &described_len);
  const char *receiver_kind = property_error_receiver_kind(receiver);

  ant_value_t error;
  if (kind == PROPERTY_ERROR_READ_ONLY) error = js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot assign to read only property '%.*s' of %s '%.*s'",
    (int)key_len, key_bytes, receiver_kind, (int)described_len, described_bytes
  );
  else if (kind == PROPERTY_ERROR_GETTER_ONLY) error = js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot set property %.*s of %.*s which has only a getter",
    (int)key_len, key_bytes, (int)described_len, described_bytes
  );
  else error = js_mkerr_typed(
    js, JS_ERR_TYPE, "Cannot create property '%.*s' on %s '%.*s'",
    (int)key_len, key_bytes, receiver_kind, (int)described_len, described_bytes
  );

  GC_ROOT_RESTORE(js, root_mark);
  return error;
}

ant_value_t Ant_Error_CreateReadOnlyProperty(ant_t *js, ant_value_t receiver, ant_value_t key) {
  return property_error(js, PROPERTY_ERROR_READ_ONLY, receiver, key);
}

ant_value_t Ant_Error_CreateGetterOnlyProperty(ant_t *js, ant_value_t receiver, ant_value_t key) {
  return property_error(js, PROPERTY_ERROR_GETTER_ONLY, receiver, key);
}

ant_value_t Ant_Error_CreatePropertyOnPrimitive(ant_t *js, ant_value_t receiver, ant_value_t key) {
  return property_error(js, PROPERTY_ERROR_PRIMITIVE, receiver, key);
}
