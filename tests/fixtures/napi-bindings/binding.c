#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct napi_env__ *napi_env;
typedef struct napi_value__ *napi_value;
typedef struct napi_callback_info__ *napi_callback_info;
typedef struct napi_async_cleanup_hook_handle__ *napi_async_cleanup_hook_handle;
typedef int napi_status;
typedef struct { uint64_t lower; uint64_t upper; } napi_type_tag;
typedef napi_value (*napi_callback)(napi_env, napi_callback_info);
typedef void (*napi_finalize)(napi_env, void *, void *);
typedef void (*node_api_noenv_finalize)(void *, void *);
typedef void (*napi_async_cleanup_hook)(napi_async_cleanup_hook_handle, void *);

extern napi_status napi_get_cb_info(napi_env, napi_callback_info, size_t *, napi_value *, napi_value *, void **);
extern napi_status napi_create_function(napi_env, const char *, size_t, napi_callback, void *, napi_value *);
extern napi_status napi_set_named_property(napi_env, napi_value, const char *, napi_value);
extern napi_status napi_get_boolean(napi_env, bool, napi_value *);
extern napi_status napi_get_null(napi_env, napi_value *);
extern napi_status napi_create_double(napi_env, double, napi_value *);
extern napi_status napi_get_value_uint32(napi_env, napi_value, uint32_t *);
extern napi_status napi_create_array(napi_env, napi_value *);
extern napi_status napi_set_element(napi_env, napi_value, uint32_t, napi_value);
extern napi_status napi_create_string_utf8(napi_env, const char *, size_t, napi_value *);
extern napi_status napi_call_function(napi_env, napi_value, napi_value, size_t, const napi_value *, napi_value *);
extern napi_status napi_get_undefined(napi_env, napi_value *);

extern napi_status napi_throw_type_error(napi_env, const char *, const char *);
extern napi_status napi_create_range_error(napi_env, napi_value, napi_value, napi_value *);
extern napi_status napi_is_error(napi_env, napi_value, bool *);
extern napi_status napi_create_string_latin1(napi_env, const char *, size_t, napi_value *);
extern napi_status napi_get_value_string_latin1(napi_env, napi_value, char *, size_t, size_t *);
extern napi_status napi_get_value_string_utf8(napi_env, napi_value, char *, size_t, size_t *);
extern napi_status napi_get_value_int32(napi_env, napi_value, int32_t *);
extern napi_status napi_get_property_names(napi_env, napi_value, napi_value *);
extern napi_status napi_has_element(napi_env, napi_value, uint32_t, bool *);
extern napi_status napi_delete_property(napi_env, napi_value, napi_value, bool *);
extern napi_status napi_create_external_buffer(napi_env, size_t, void *, napi_finalize, void *, napi_value *);
extern napi_status napi_create_object(napi_env, napi_value *);
typedef struct napi_escapable_handle_scope__ *napi_escapable_handle_scope;
extern napi_status napi_open_escapable_handle_scope(napi_env, napi_escapable_handle_scope *);
extern napi_status napi_close_escapable_handle_scope(napi_env, napi_escapable_handle_scope);
extern napi_status napi_escape_handle(napi_env, napi_escapable_handle_scope, napi_value, napi_value *);
extern napi_status napi_type_tag_object(napi_env, napi_value, const napi_type_tag *);
extern napi_status napi_check_object_type_tag(napi_env, napi_value, const napi_type_tag *, bool *);
extern napi_status napi_object_freeze(napi_env, napi_value);
extern napi_status napi_object_seal(napi_env, napi_value);
extern napi_status node_api_set_prototype(napi_env, napi_value, napi_value);
extern napi_status node_api_create_object_with_properties(napi_env, napi_value, const napi_value *, const napi_value *, size_t, napi_value *);
extern napi_status napi_create_string_utf16(napi_env, const uint16_t *, size_t, napi_value *);
extern napi_status node_api_create_property_key_latin1(napi_env, const char *, size_t, napi_value *);
extern napi_status node_api_create_property_key_utf8(napi_env, const char *, size_t, napi_value *);
extern napi_status node_api_create_property_key_utf16(napi_env, const uint16_t *, size_t, napi_value *);
extern napi_status node_api_create_external_string_latin1(napi_env, char *, size_t, napi_finalize, void *, napi_value *, bool *);
extern napi_status node_api_create_external_string_utf16(napi_env, uint16_t *, size_t, napi_finalize, void *, napi_value *, bool *);
extern napi_status node_api_symbol_for(napi_env, const char *, size_t, napi_value *);
extern napi_status node_api_create_syntax_error(napi_env, napi_value, napi_value, napi_value *);
extern napi_status node_api_throw_syntax_error(napi_env, const char *, const char *);
extern napi_status napi_create_dataview(napi_env, size_t, napi_value, size_t, napi_value *);
extern napi_status napi_get_dataview_info(napi_env, napi_value, size_t *, void **, napi_value *, size_t *);
extern napi_status napi_detach_arraybuffer(napi_env, napi_value);
extern napi_status napi_is_detached_arraybuffer(napi_env, napi_value, bool *);
extern napi_status node_api_create_buffer_from_arraybuffer(napi_env, napi_value, size_t, size_t, napi_value *);
extern napi_status node_api_create_sharedarraybuffer(napi_env, size_t, void **, napi_value *);
extern napi_status node_api_create_external_sharedarraybuffer(napi_env, void *, size_t, node_api_noenv_finalize, void *, napi_value *);
extern napi_status node_api_is_sharedarraybuffer(napi_env, napi_value, bool *);
extern napi_status node_api_get_module_file_name(napi_env, const char **);
extern napi_status node_api_post_finalizer(napi_env, napi_finalize, void *, void *);
extern napi_status napi_add_async_cleanup_hook(napi_env, napi_async_cleanup_hook, void *, napi_async_cleanup_hook_handle *);
extern napi_status napi_remove_async_cleanup_hook(napi_async_cleanup_hook_handle);

#define AUTO_LENGTH SIZE_MAX

static const napi_type_tag tags[2] = {
  { 0x1111111111111111ull, 0x2222222222222222ull },
  { 0x1111111111111111ull, 0x3333333333333333ull },
};

static uint16_t utf16_text[] = { 'h', 0xe9, 0xd83d, 0xde00, 0 };
static char latin1_text[] = { 'c', 'a', 'f', (char)0xe9, 0 };
static uint8_t external_bytes[4] = { 9, 8, 7, 6 };
static int string_finalized = 0;
static int posted_finalized = 0;

static void args(napi_env env, napi_callback_info info, napi_value *argv, size_t count) {
  size_t argc = count;
  napi_get_cb_info(env, info, &argc, argv, NULL, NULL);
}

static napi_value boolean(napi_env env, bool value) {
  napi_value result = NULL;
  napi_get_boolean(env, value, &result);
  return result;
}

static napi_value number(napi_env env, double value) {
  napi_value result = NULL;
  napi_create_double(env, value, &result);
  return result;
}

static uint32_t u32(napi_env env, napi_value value) {
  uint32_t result = 0;
  napi_get_value_uint32(env, value, &result);
  return result;
}

static napi_value zero(napi_env env, napi_callback_info info) {
  return number(env, 0);
}

static napi_value nothing(napi_env env, napi_callback_info info) {
  return NULL;
}

static napi_value identity(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  args(env, info, argv, 1);
  return argv[0];
}

static napi_value call_with_zero(napi_env env, napi_callback_info info) {
  napi_value argv[1], recv = NULL, result = NULL;
  args(env, info, argv, 1);
  napi_value call_argv[2] = { number(env, 0), number(env, 7) };
  napi_get_undefined(env, &recv);
  if (napi_call_function(env, recv, argv[0], 2, call_argv, &result) != 0) return NULL;
  return result;
}

static napi_value throw_coded(napi_env env, napi_callback_info info) {
  napi_throw_type_error(env, "ERR_ANT_TEST", "coded failure");
  return NULL;
}

static napi_value create_coded(napi_env env, napi_callback_info info) {
  napi_value argv[2], result = NULL;
  args(env, info, argv, 2);
  if (napi_create_range_error(env, argv[0], argv[1], &result) != 0) return NULL;
  return result;
}

static napi_value is_error(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  bool result = false;
  args(env, info, argv, 1);
  if (napi_is_error(env, argv[0], &result) != 0) return NULL;
  return boolean(env, result);
}

static napi_value latin1(napi_env env, napi_callback_info info) {
  napi_value result = NULL;
  napi_create_string_latin1(env, latin1_text, 4, &result);
  return result;
}

static napi_value read_latin1(napi_env env, napi_callback_info info) {
  napi_value argv[1], result = NULL;
  char buffer[16];
  size_t length = 0;
  args(env, info, argv, 1);
  if (napi_get_value_string_latin1(env, argv[0], buffer, sizeof(buffer), &length) != 0) return NULL;

  napi_create_array(env, &result);
  for (size_t i = 0; i < length; i++) napi_set_element(env, result, (uint32_t)i, number(env, (unsigned char)buffer[i]));
  return result;
}

static napi_value read_utf8(napi_env env, napi_callback_info info) {
  napi_value argv[2], result = NULL;
  char buffer[32];
  size_t length = 0;
  args(env, info, argv, 2);
  if (napi_get_value_string_utf8(env, argv[0], buffer, u32(env, argv[1]), &length) != 0) return NULL;
  napi_create_string_utf8(env, buffer, length, &result);
  return result;
}

static napi_value to_int32(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  int32_t result = 1;
  args(env, info, argv, 1);
  if (napi_get_value_int32(env, argv[0], &result) != 0) return NULL;
  return number(env, result);
}

static napi_value property_names(napi_env env, napi_callback_info info) {
  napi_value argv[1], result = NULL;
  args(env, info, argv, 1);
  if (napi_get_property_names(env, argv[0], &result) != 0) return NULL;
  return result;
}

static napi_value has_element(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  bool result = false;
  args(env, info, argv, 2);
  if (napi_has_element(env, argv[0], u32(env, argv[1]), &result) != 0) return NULL;
  return boolean(env, result);
}

static napi_value delete_property(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  bool result = false;
  args(env, info, argv, 2);
  if (napi_delete_property(env, argv[0], argv[1], &result) != 0) return NULL;
  return boolean(env, result);
}

static uint8_t external_buffer_bytes[4] = { 1, 2, 3, 4 };

static void external_buffer_finalize(napi_env env, void *data, void *hint) {}

static napi_value external_buffer(napi_env env, napi_callback_info info) {
  napi_value result = NULL;
  if (napi_create_external_buffer(
    env, sizeof(external_buffer_bytes), external_buffer_bytes, external_buffer_finalize, NULL, &result
  ) != 0) return NULL;
  return result;
}

static napi_value external_buffer_byte(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  args(env, info, argv, 1);
  return number(env, external_buffer_bytes[u32(env, argv[0]) & 3]);
}

static napi_value escape(napi_env env, napi_callback_info info) {
  napi_escapable_handle_scope scope = NULL;
  napi_value inner = NULL, outer = NULL, again = NULL;

  if (napi_open_escapable_handle_scope(env, &scope) != 0) return NULL;
  napi_create_object(env, &inner);
  napi_set_named_property(env, inner, "escaped", boolean(env, true));
  napi_status first = napi_escape_handle(env, scope, inner, &outer);
  napi_status second = napi_escape_handle(env, scope, inner, &again);
  napi_close_escapable_handle_scope(env, scope);

  return first == 0 && second != 0 ? outer : NULL;
}

static napi_value tag(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  args(env, info, argv, 2);
  return boolean(env, napi_type_tag_object(env, argv[0], &tags[u32(env, argv[1])]) == 0);
}

static napi_value check_tag(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  bool matches = true;
  args(env, info, argv, 2);
  if (napi_check_object_type_tag(env, argv[0], &tags[u32(env, argv[1])], &matches) != 0) return NULL;
  return boolean(env, matches);
}

static napi_value freeze(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  args(env, info, argv, 1);
  return boolean(env, napi_object_freeze(env, argv[0]) == 0);
}

static napi_value seal(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  args(env, info, argv, 1);
  return boolean(env, napi_object_seal(env, argv[0]) == 0);
}

static napi_value set_prototype(napi_env env, napi_callback_info info) {
  napi_value argv[2];
  args(env, info, argv, 2);
  return boolean(env, node_api_set_prototype(env, argv[0], argv[1]) == 0);
}

static napi_value create_with(napi_env env, napi_callback_info info) {
  napi_value argv[5], result = NULL;
  args(env, info, argv, 5);
  napi_value names[2] = { argv[1], argv[3] };
  napi_value values[2] = { argv[2], argv[4] };
  if (node_api_create_object_with_properties(env, argv[0], names, values, 2, &result) != 0) return NULL;
  return result;
}

static napi_value utf16(napi_env env, napi_callback_info info) {
  napi_value result = NULL;
  napi_create_string_utf16(env, utf16_text, AUTO_LENGTH, &result);
  return result;
}

static napi_value keys(napi_env env, napi_callback_info info) {
  napi_value result = NULL, key = NULL;
  napi_create_array(env, &result);
  if (node_api_create_property_key_latin1(env, latin1_text, 4, &key) == 0) napi_set_element(env, result, 0, key);
  if (node_api_create_property_key_utf8(env, "k\xc3\xa9y", AUTO_LENGTH, &key) == 0) napi_set_element(env, result, 1, key);
  if (node_api_create_property_key_utf16(env, utf16_text, 4, &key) == 0) napi_set_element(env, result, 2, key);
  return result;
}

static void string_finalize(napi_env env, void *data, void *hint) {
  string_finalized++;
}

static napi_value external_string(napi_env env, napi_callback_info info) {
  napi_value argv[1], result = NULL, value = NULL;
  bool copied = false;
  args(env, info, argv, 1);
  string_finalized = 0;

  napi_status status = u32(env, argv[0]) == 0
    ? node_api_create_external_string_latin1(env, latin1_text, 4, string_finalize, NULL, &value, &copied)
    : node_api_create_external_string_utf16(env, utf16_text, 4, string_finalize, NULL, &value, &copied);
  if (status != 0) return NULL;

  napi_create_array(env, &result);
  napi_set_element(env, result, 0, value);
  napi_set_element(env, result, 1, boolean(env, copied));
  napi_set_element(env, result, 2, number(env, string_finalized));
  return result;
}

static napi_value symbol_for(napi_env env, napi_callback_info info) {
  napi_value result = NULL;
  node_api_symbol_for(env, "ant.napi.test", AUTO_LENGTH, &result);
  return result;
}

static napi_value syntax_error(napi_env env, napi_callback_info info) {
  napi_value argv[1], result = NULL;
  args(env, info, argv, 1);
  node_api_create_syntax_error(env, NULL, argv[0], &result);
  return result;
}

static napi_value throw_syntax(napi_env env, napi_callback_info info) {
  node_api_throw_syntax_error(env, NULL, "bad syntax");
  return NULL;
}

static napi_value data_view(napi_env env, napi_callback_info info) {
  napi_value argv[3], result = NULL;
  args(env, info, argv, 3);
  if (napi_create_dataview(env, u32(env, argv[2]), argv[0], u32(env, argv[1]), &result) != 0) return NULL;
  return result;
}

static napi_value data_view_info(napi_env env, napi_callback_info info) {
  napi_value argv[1], result = NULL, buffer = NULL;
  size_t length = 0, offset = 0;
  void *data = NULL;
  args(env, info, argv, 1);
  if (napi_get_dataview_info(env, argv[0], &length, &data, &buffer, &offset) != 0) return NULL;

  napi_create_array(env, &result);
  napi_set_element(env, result, 0, number(env, (double)length));
  napi_set_element(env, result, 1, number(env, (double)offset));
  napi_set_element(env, result, 2, buffer);
  napi_set_element(env, result, 3, number(env, data ? *(uint8_t *)data : -1));
  return result;
}

static napi_value detach(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  args(env, info, argv, 1);
  return boolean(env, napi_detach_arraybuffer(env, argv[0]) == 0);
}

static napi_value is_detached(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  bool detached = true;
  args(env, info, argv, 1);
  if (napi_is_detached_arraybuffer(env, argv[0], &detached) != 0) return NULL;
  return boolean(env, detached);
}

static napi_value buffer_from(napi_env env, napi_callback_info info) {
  napi_value argv[3], result = NULL;
  args(env, info, argv, 3);
  node_api_create_buffer_from_arraybuffer(env, argv[0], u32(env, argv[1]), u32(env, argv[2]), &result);
  return result;
}

static napi_value shared_buffer(napi_env env, napi_callback_info info) {
  napi_value argv[1], result = NULL;
  void *data = NULL;
  args(env, info, argv, 1);
  uint32_t length = u32(env, argv[0]);
  if (node_api_create_sharedarraybuffer(env, length, &data, &result) != 0) return NULL;
  for (uint32_t i = 0; i < length; i++) ((uint8_t *)data)[i] = (uint8_t)(i + 1);
  return result;
}

static void external_bytes_finalize(void *data, void *hint) {}

static napi_value external_shared_buffer(napi_env env, napi_callback_info info) {
  napi_value result = NULL;
  if (node_api_create_external_sharedarraybuffer(
    env, external_bytes, sizeof(external_bytes), external_bytes_finalize, NULL, &result
  ) != 0) return NULL;
  return result;
}

static napi_value is_shared(napi_env env, napi_callback_info info) {
  napi_value argv[1];
  bool shared = true;
  args(env, info, argv, 1);
  if (node_api_is_sharedarraybuffer(env, argv[0], &shared) != 0) return NULL;
  return boolean(env, shared);
}

static napi_value module_file_name(napi_env env, napi_callback_info info) {
  napi_value result = NULL;
  const char *name = NULL;
  if (node_api_get_module_file_name(env, &name) != 0) return NULL;
  if (name) napi_create_string_utf8(env, name, AUTO_LENGTH, &result);
  else napi_get_null(env, &result);
  return result;
}

static void posted_finalize(napi_env env, void *data, void *hint) {
  posted_finalized += *(int *)data;
}

static napi_value post_finalizer(napi_env env, napi_callback_info info) {
  static int increment = 1;
  return boolean(env, node_api_post_finalizer(env, posted_finalize, &increment, NULL) == 0);
}

static napi_value posted(napi_env env, napi_callback_info info) {
  return boolean(env, posted_finalized == 1);
}

static void async_cleanup(napi_async_cleanup_hook_handle handle, void *data) {
  fputs((const char *)data, stdout);
  fflush(stdout);
  napi_remove_async_cleanup_hook(handle);
}

static napi_value install_async_cleanup(napi_env env, napi_callback_info info) {
  return boolean(env, napi_add_async_cleanup_hook(env, async_cleanup, "async cleanup hook ran\n", NULL) == 0);
}

static napi_value add_remove_async_cleanup(napi_env env, napi_callback_info info) {
  napi_async_cleanup_hook_handle handle = NULL;
  napi_status status = napi_add_async_cleanup_hook(env, async_cleanup, "removed hook ran\n", &handle);
  if (status != 0 || !handle) return boolean(env, false);
  return boolean(env, napi_remove_async_cleanup_hook(handle) == 0);
}

__attribute__((visibility("default")))
napi_value napi_register_module_v1(napi_env env, napi_value exports) {
  static const struct { const char *name; napi_callback cb; } methods[] = {
    { "zero", zero },
    { "nothing", nothing },
    { "identity", identity },
    { "callWithZero", call_with_zero },
    { "throwCoded", throw_coded },
    { "createCoded", create_coded },
    { "isError", is_error },
    { "latin1", latin1 },
    { "readLatin1", read_latin1 },
    { "readUtf8", read_utf8 },
    { "toInt32", to_int32 },
    { "propertyNames", property_names },
    { "hasElement", has_element },
    { "deleteProperty", delete_property },
    { "externalBuffer", external_buffer },
    { "externalBufferByte", external_buffer_byte },
    { "escape", escape },
    { "tag", tag },
    { "checkTag", check_tag },
    { "freeze", freeze },
    { "seal", seal },
    { "setPrototype", set_prototype },
    { "createWith", create_with },
    { "utf16", utf16 },
    { "keys", keys },
    { "externalString", external_string },
    { "symbolFor", symbol_for },
    { "syntaxError", syntax_error },
    { "throwSyntax", throw_syntax },
    { "dataView", data_view },
    { "dataViewInfo", data_view_info },
    { "detach", detach },
    { "isDetached", is_detached },
    { "bufferFrom", buffer_from },
    { "sharedBuffer", shared_buffer },
    { "externalSharedBuffer", external_shared_buffer },
    { "isShared", is_shared },
    { "moduleFileName", module_file_name },
    { "postFinalizer", post_finalizer },
    { "posted", posted },
    { "installAsyncCleanup", install_async_cleanup },
    { "addRemoveAsyncCleanup", add_remove_async_cleanup },
  };

  for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
    napi_value fn = NULL;
    if (napi_create_function(env, methods[i].name, AUTO_LENGTH, methods[i].cb, NULL, &fn) == 0)
      napi_set_named_property(env, exports, methods[i].name, fn);
  }
  return exports;
}
