#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "ant.h"
#include "cfunc.h"
#include "errors.h"
#include "internal.h"
#include "descriptors.h"
#include "gc/roots.h"

#include "silver/call.h"
#include "modules/url.h"
#include "modules/symbol.h"
#include "modules/iterator.h"
#include "url_internal.h"

enum {
  USP_ITER_ENTRIES = 0,
  USP_ITER_KEYS = 1,
  USP_ITER_VALUES = 2
};

static const char USP_NAME_REQUIRED[] = "The \"name\" argument must be specified";
static const char USP_NAME_VALUE_REQUIRED[] = "The \"name\" and \"value\" arguments must be specified";

bool usp_is_urlsearchparams(ant_t *js, ant_value_t obj) {
  return js_check_brand(obj, BRAND_URLSEARCHPARAMS);
}

static ant_value_t usp_entries(ant_value_t usp) {
  ant_value_t entries = js_get_slot(usp, SLOT_ENTRIES);
  return is_special_object(entries) ? entries : 0;
}

static ant_offset_t usp_entry_count(ant_t *js, ant_value_t entries) {
  return entries ? js_arr_len(js, entries) : 0;
}

static ant_value_t usp_make_entry(ant_t *js, ant_value_t key, ant_value_t value) {
  ant_value_t entry = js_mkarr(js);
  js_arr_push(js, entry, key);
  js_arr_push(js, entry, value);
  return entry;
}

static const char *usp_entry_key(ant_t *js, ant_value_t entry) {
  return js_getstr(js, js_arr_get(js, entry, 0), NULL);
}

static const char *usp_entry_value(ant_t *js, ant_value_t entry) {
  return js_getstr(js, js_arr_get(js, entry, 1), NULL);
}

static bool usp_entry_has_key(ant_t *js, ant_value_t entry, const char *key) {
  const char *entry_key = usp_entry_key(js, entry);
  return entry_key && strcmp(entry_key, key) == 0;
}

static bool usp_entry_matches(ant_t *js, ant_value_t entry, const char *key, const char *value) {
  if (!usp_entry_has_key(js, entry, key)) return false;
  if (!value) return true;
  const char *entry_value = usp_entry_value(js, entry);
  return entry_value && strcmp(entry_value, value) == 0;
}

static ant_value_t usp_optional_value(ant_t *js, ant_value_t *args, int nargs, const char **out) {
  *out = NULL;
  if (nargs < 2 || is_undefined(args[1])) return js_mkundef();

  ant_value_t value = url_string_value(js, args[1]);
  if (is_err(value)) return value;

  *out = js_getstr(js, value, NULL);
  return js_mkundef();
}

static void usp_push_decoded_pair(ant_t *js, ant_value_t arr, const char *start, size_t len) {
  char *pair = strndup(start, len);
  if (!pair) return;

  char *eq = strchr(pair, '=');
  char *raw_value = "";
  if (eq) {
    *eq = '\0';
    raw_value = eq + 1;
  }

  char *key = form_urldecode(pair);
  char *value = form_urldecode(raw_value);
  js_arr_push(js, arr, usp_make_entry(js, js_mkstr(js, key, strlen(key)), js_mkstr(js, value, strlen(value))));

  free(pair);
  free(key);
  free(value);
}

static ant_value_t parse_query_to_arr(ant_t *js, const char *query) {
  ant_value_t arr = js_mkarr(js);
  if (!query) return arr;

  for (const char *p = query; *p;) {
    const char *amp = strchr(p, '&');
    size_t len = amp ? (size_t)(amp - p) : strlen(p);
    if (len > 0) usp_push_decoded_pair(js, arr, p, len);
    p = amp ? amp + 1 : p + (len > 0 ? len : 1);
  }

  return arr;
}

static ant_value_t parse_query_string_init(ant_t *js, const char *query) {
  return parse_query_to_arr(js, query && query[0] == '?' ? query + 1 : query);
}

char *usp_serialize(ant_t *js, ant_value_t usp) {
  ant_value_t entries = usp_entries(usp);
  ant_offset_t len = usp_entry_count(js, entries);

  size_t cap = 256;
  char *buf = malloc(cap);
  if (!buf) return strdup("");
  size_t pos = 0;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    size_t key_len = 0, value_len = 0;

    char *key = js_getstr(js, js_arr_get(js, entry, 0), &key_len);
    char *value = js_getstr(js, js_arr_get(js, entry, 1), &value_len);
    char *encoded_key = key ? form_urlencode_n(key, key_len) : strdup("");
    char *encoded_value = value ? form_urlencode_n(value, value_len) : strdup("");

    size_t needed = strlen(encoded_key) + strlen(encoded_value) + 3;
    if (pos + needed >= cap) {
      cap = cap * 2 + needed;
      char *grown = realloc(buf, cap);
      if (!grown) {
        free(buf);
        free(encoded_key);
        free(encoded_value);
        return strdup("");
      }
      buf = grown;
    }

    if (pos > 0) buf[pos++] = '&';
    pos += (size_t)sprintf(buf + pos, "%s=%s", encoded_key, encoded_value);
    free(encoded_key);
    free(encoded_value);
  }

  buf[pos] = '\0';
  return buf;
}

static void usp_push_to_url(ant_t *js, ant_value_t usp) {
  ant_value_t url_obj = js_get_slot(usp, SLOT_DATA);
  if (!is_special_object(url_obj)) return;

  url_state_t *s = url_get_state(url_obj);
  if (!s || !s->href) return;

  ada_url url = ada_parse(s->href, strlen(s->href));
  if (!url) return;
  if (!ada_is_valid(url)) {
    ada_free(url);
    return;
  }

  char *qs = usp_serialize(js, usp);
  if (*qs) ada_set_search(url, qs, strlen(qs));
  else ada_clear_search(url);
  free(qs);

  url_state_t next = {0};
  if (ada_url_to_state(url, &next) == 0) {
    url_state_clear(s);
    *s = next;
  }

  ada_free(url);
}

static void usp_replace_entries(ant_t *js, ant_value_t usp, ant_value_t entries) {
  js_set_slot_wb(js, usp, SLOT_ENTRIES, entries);
  usp_push_to_url(js, usp);
}

void url_sync_usp(ant_t *js, ant_value_t url_obj, const char *query) {
  ant_value_t usp = js_get_slot(url_obj, SLOT_ENTRIES);
  if (!is_special_object(usp)) return;
  js_set_slot_wb(js, usp, SLOT_ENTRIES, parse_query_to_arr(js, query));
}

static ant_value_t usp_new(ant_t *js, ant_value_t url_obj, ant_value_t entries) {
  ant_value_t usp = Ant_Object_CreateBare(js);
  js_set_proto_init(usp, js->builtins.usp_proto);
  js_set_slot(usp, SLOT_BRAND, js_mknum(BRAND_URLSEARCHPARAMS));
  js_set_slot_wb(js, usp, SLOT_DATA, url_obj);
  js_set_slot(usp, SLOT_ENTRIES, entries);
  return usp;
}

ant_value_t make_usp_for_url(ant_t *js, ant_value_t url_obj, const char *query) {
  return usp_new(js, url_obj, parse_query_to_arr(js, query));
}

static ant_value_t usp_get(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, USP_NAME_REQUIRED);

  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;

  const char *key = js_getstr(js, key_sv, NULL);
  ant_value_t entries = usp_entries(js->this_val);
  if (!key || !entries) return js_mknull();

  ant_offset_t len = js_arr_len(js, entries);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    if (usp_entry_has_key(js, entry, key)) return js_arr_get(js, entry, 1);
  }

  return js_mknull();
}

static ant_value_t usp_getAll(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, USP_NAME_REQUIRED);

  ant_value_t result = js_mkarr(js);
  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;

  const char *key = js_getstr(js, key_sv, NULL);
  ant_value_t entries = usp_entries(js->this_val);
  if (!key || !entries) return result;

  ant_offset_t len = js_arr_len(js, entries);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    if (usp_entry_has_key(js, entry, key)) js_arr_push(js, result, js_arr_get(js, entry, 1));
  }

  return result;
}

static ant_value_t usp_has(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, USP_NAME_REQUIRED);

  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;

  const char *key = js_getstr(js, key_sv, NULL);
  if (!key) return js_false;

  const char *value = NULL;
  ant_value_t converted = usp_optional_value(js, args, nargs, &value);
  if (is_err(converted)) return converted;

  ant_value_t entries = usp_entries(js->this_val);
  ant_offset_t len = usp_entry_count(js, entries);
  for (ant_offset_t i = 0; i < len; i++)
    if (usp_entry_matches(js, js_arr_get(js, entries, i), key, value)) return js_true;

  return js_false;
}

static ant_value_t usp_set(ant_params_t) {
  if (nargs < 2) return url_missing_args_error(js, USP_NAME_VALUE_REQUIRED);

  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;

  ant_value_t value_sv = url_string_value(js, args[1]);
  if (is_err(value_sv)) return value_sv;

  const char *key = js_getstr(js, key_sv, NULL);
  if (!key) return js_mkundef();

  ant_value_t entries = usp_entries(js->this_val);
  ant_offset_t len = usp_entry_count(js, entries);
  ant_value_t next = js_mkarr(js);
  bool found = false;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    if (!usp_entry_has_key(js, entry, key)) {
      js_arr_push(js, next, entry);
      continue;
    }
    if (found) continue;
    js_arr_push(js, next, usp_make_entry(js, key_sv, value_sv));
    found = true;
  }

  if (!found) js_arr_push(js, next, usp_make_entry(js, key_sv, value_sv));
  usp_replace_entries(js, js->this_val, next);

  return js_mkundef();
}

static ant_value_t usp_append(ant_params_t) {
  if (nargs < 2) return url_missing_args_error(js, USP_NAME_VALUE_REQUIRED);

  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;

  ant_value_t value_sv = url_string_value(js, args[1]);
  if (is_err(value_sv)) return value_sv;

  ant_value_t entries = usp_entries(js->this_val);
  if (!entries) return js_mkundef();

  js_arr_push(js, entries, usp_make_entry(js, key_sv, value_sv));
  usp_push_to_url(js, js->this_val);

  return js_mkundef();
}

static ant_value_t usp_delete(ant_params_t) {
  if (nargs < 1) return url_missing_args_error(js, USP_NAME_REQUIRED);

  ant_value_t key_sv = url_string_value(js, args[0]);
  if (is_err(key_sv)) return key_sv;

  const char *key = js_getstr(js, key_sv, NULL);
  if (!key) return js_mkundef();

  const char *value = NULL;
  ant_value_t converted = usp_optional_value(js, args, nargs, &value);
  if (is_err(converted)) return converted;

  ant_value_t entries = usp_entries(js->this_val);
  ant_offset_t len = usp_entry_count(js, entries);
  ant_value_t next = js_mkarr(js);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    if (!usp_entry_matches(js, entry, key, value)) js_arr_push(js, next, entry);
  }

  usp_replace_entries(js, js->this_val, next);
  return js_mkundef();
}

static ant_value_t usp_toString(ant_params_t) {
  char *s = usp_serialize(js, js->this_val);
  ant_value_t ret = js_mkstr(js, s, strlen(s));
  free(s);
  return ret;
}

static ant_value_t usp_callback_type_error(ant_t *js, ant_value_t value) {
  char received[160], message[256];
  js_describe_received(js, value, received, sizeof(received));
  snprintf(message, sizeof(message), "The \"callback\" argument must be of type function. %s", received);
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_ARG_TYPE", message);
}

static ant_value_t usp_forEach(ant_params_t) {
  if (nargs < 1 || !is_callable(args[0])) return usp_callback_type_error(js, nargs > 0 ? args[0] : js_mkundef());

  ant_value_t callback = args[0];
  ant_value_t this_arg = nargs >= 2 ? args[1] : js_mkundef();
  ant_value_t self = js->this_val;
  ant_value_t entries = usp_entries(self);
  ant_offset_t len = usp_entry_count(js, entries);

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, entries, i);
    ant_value_t call_args[3] = { js_arr_get(js, entry, 1), js_arr_get(js, entry, 0), self };
    ant_value_t result = sv_vm_call(js->vm, js, callback, this_arg, call_args, 3, NULL, js_mkundef());
    if (is_err(result)) return result;
  }

  return js_mkundef();
}

static ant_value_t usp_size_get(ant_params_t) {
  return js_mknum((double)usp_entry_count(js, usp_entries(js->this_val)));
}

static void usp_sort_entries(ant_t *js, ant_value_t *items, ant_offset_t len) {
  for (ant_offset_t i = 1; i < len; i++) {
    ant_value_t cur = items[i];
    const char *cur_key = usp_entry_key(js, cur);
    ant_offset_t j = i;

    while (j > 0) {
      const char *prev_key = usp_entry_key(js, items[j - 1]);
      if (strcmp(prev_key ? prev_key : "", cur_key ? cur_key : "") <= 0) break;
      items[j] = items[j - 1];
      j--;
    }

    items[j] = cur;
  }
}

static ant_value_t usp_sort(ant_params_t) {
  ant_value_t entries = usp_entries(js->this_val);
  ant_offset_t len = usp_entry_count(js, entries);
  if (len <= 1) return js_mkundef();

  ant_value_t *items = malloc(sizeof(ant_value_t) * (size_t)len);
  if (!items) return js_mkundef();

  for (ant_offset_t i = 0; i < len; i++) items[i] = js_arr_get(js, entries, i);
  usp_sort_entries(js, items, len);

  ant_value_t next = js_mkarr(js);
  for (ant_offset_t i = 0; i < len; i++) js_arr_push(js, next, items[i]);
  free(items);

  usp_replace_entries(js, js->this_val, next);
  return js_mkundef();
}

static ant_value_t usp_iter_value(ant_t *js, uint32_t kind, ant_value_t entry) {
  ant_value_t key = js_arr_get(js, entry, 0);
  ant_value_t value = js_arr_get(js, entry, 1);
  if (kind == USP_ITER_KEYS) return key;
  if (kind == USP_ITER_VALUES) return value;
  return usp_make_entry(js, key, value);
}

static ant_value_t usp_iter_next(ant_params_t) {
  ant_value_t state_v = js_get_slot(js->this_val, SLOT_ITER_STATE);
  if (vtype(state_v) != kTypeNumber) return js_iter_result(js, false, js_mkundef());

  uint32_t state = (uint32_t)js_getnum(state_v);
  uint32_t kind = iter_state_kind(state);
  uint32_t idx = iter_state_index(state);

  ant_value_t entries = usp_entries(js_get_slot(js->this_val, SLOT_DATA));
  if ((ant_offset_t)idx >= usp_entry_count(js, entries)) return js_iter_result(js, false, js_mkundef());

  js_set_slot(js->this_val, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, idx + 1)));
  return js_iter_result(js, true, usp_iter_value(js, kind, js_arr_get(js, entries, (ant_offset_t)idx)));
}

static ant_value_t make_usp_iter(ant_t *js, ant_value_t usp, int kind) {
  ant_value_t iter = Ant_Object_CreateBare(js);
  js_set_proto_init(iter, js->builtins.usp_iter_proto);
  js_set_slot_wb(js, iter, SLOT_DATA, usp);
  js_set_slot(iter, SLOT_ITER_STATE, js_mknum((double)iter_state_pack(kind, 0)));
  return iter;
}

static ant_value_t usp_entries_fn(ant_params_t) {
  return make_usp_iter(js, js->this_val, USP_ITER_ENTRIES);
}

static ant_value_t usp_keys_fn(ant_params_t) {
  return make_usp_iter(js, js->this_val, USP_ITER_KEYS);
}

static ant_value_t usp_values_fn(ant_params_t) {
  return make_usp_iter(js, js->this_val, USP_ITER_VALUES);
}

static ant_value_t usp_invalid_tuple(ant_t *js) {
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_INVALID_TUPLE", "Each query pair must be an iterable [name, value] tuple");
}

static ant_value_t usp_not_iterable(ant_t *js) {
  return js_mkerr_code(js, JS_ERR_TYPE, "ERR_ARG_NOT_ITERABLE", "Query pairs must be iterable");
}

static ant_value_t usp_pair_items_from_iterable(ant_t *js, ant_value_t pair, ant_value_t items[2]) {
  iterator_t it;
  if (!js_iter_open(js, pair, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : usp_invalid_tuple(js);

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, it.iterator);
  GC_ROOT_PIN(js, it.next_fn);

  size_t count = 0;
  ant_value_t value = js_mkundef();
  while (js_iter_next(js, &it, &value)) {
    if (count < 2) {
      items[count] = value;
      GC_ROOT_PIN(js, items[count]);
    }
    count++;
  }

  GC_ROOT_RESTORE(js, root_mark);
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  return count == 2 ? js_mkundef() : usp_invalid_tuple(js);
}

static ant_value_t usp_pair_items(ant_t *js, ant_value_t pair, ant_value_t items[2]) {
  if (vtype(pair) != kTypeArray || is_proxy(pair)) return usp_pair_items_from_iterable(js, pair, items);
  if (js_arr_len(js, pair) != 2) return usp_invalid_tuple(js);

  items[0] = js_arr_get(js, pair, 0);
  items[1] = js_arr_get(js, pair, 1);
  return js_mkundef();
}

static ant_value_t usp_add_pair(ant_t *js, ant_value_t entries, ant_value_t pair) {
  if (!is_object_type(pair) && !is_callable(pair)) return usp_invalid_tuple(js);

  ant_value_t items[2] = { js_mkundef(), js_mkundef() };
  ant_value_t status = usp_pair_items(js, pair, items);
  if (is_err(status)) return status;

  ant_value_t key = url_string_value(js, items[0]);
  if (is_err(key)) return key;

  ant_value_t value = url_string_value(js, items[1]);
  if (is_err(value)) return value;

  js_arr_push(js, entries, usp_make_entry(js, key, value));
  return js_mkundef();
}

static ant_value_t usp_init_from_primitive(ant_t *js, ant_value_t obj, ant_value_t init) {
  ant_value_t init_sv = url_string_value(js, init);
  if (is_err(init_sv)) return init_sv;

  const char *s = js_getstr(js, init_sv, NULL);
  if (s) js_set_slot(obj, SLOT_ENTRIES, parse_query_string_init(js, s));
  return obj;
}

static ant_value_t usp_init_from_usp(ant_t *js, ant_value_t obj, ant_value_t entries, ant_value_t init) {
  ant_value_t src = js_get_slot(init, SLOT_ENTRIES);
  ant_offset_t len = vtype(src) == kTypeArray ? js_arr_len(js, src) : 0;

  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t entry = js_arr_get(js, src, i);
    js_arr_push(js, entries, usp_make_entry(js, js_arr_get(js, entry, 0), js_arr_get(js, entry, 1)));
  }

  return obj;
}

static ant_value_t usp_add_array_pairs(ant_t *js, ant_value_t entries, ant_value_t init) {
  ant_offset_t len = js_arr_len(js, init);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t status = usp_add_pair(js, entries, js_arr_get(js, init, i));
    if (is_err(status)) return status;
  }
  return js_mkundef();
}

static ant_value_t usp_add_iterated_pairs(ant_t *js, ant_value_t entries, ant_value_t init) {
  iterator_t it;
  if (!js_iter_open(js, init, &it))
    return Ant_Exception_Pending(js) ? Ant_Exception_Current(js) : usp_not_iterable(js);

  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, it.iterator);
  GC_ROOT_PIN(js, it.next_fn);

  ant_value_t status = js_mkundef();
  ant_value_t pair = js_mkundef();
  while (js_iter_next(js, &it, &pair)) {
    status = usp_add_pair(js, entries, pair);
    if (!is_err(status)) continue;
    js_iter_close(js, &it);
    break;
  }

  GC_ROOT_RESTORE(js, root_mark);
  if (!is_err(status) && Ant_Exception_Pending(js)) return Ant_Exception_Current(js);
  return status;
}

static ant_value_t usp_init_from_iterable(ant_t *js, ant_value_t obj, ant_value_t entries, ant_value_t init) {
  GC_ROOT_SAVE(root_mark, js);
  GC_ROOT_PIN(js, obj);
  GC_ROOT_PIN(js, entries);

  ant_value_t status = vtype(init) == kTypeArray && !is_proxy(init)
    ? usp_add_array_pairs(js, entries, init)
    : usp_add_iterated_pairs(js, entries, init);

  GC_ROOT_RESTORE(js, root_mark);
  return is_err(status) ? status : obj;
}

static ant_value_t usp_init_from_record(ant_t *js, ant_value_t obj, ant_value_t entries, ant_value_t init) {
  ant_iter_t it = js_prop_iter_begin(js, init);
  const char *key;
  size_t key_len;
  ant_value_t val;
  ant_value_t result = obj;

  while (js_prop_iter_next(&it, &key, &key_len, &val)) {
    val = js_getprop_fallback_len(js, init, key, key_len);
    if (is_err(val)) {
      result = val;
      break;
    }

    ant_value_t value = url_string_value(js, val);
    if (is_err(value)) {
      result = value;
      break;
    }

    js_arr_push(js, entries, usp_make_entry(js, js_mkstr(js, key, key_len), value));
  }

  js_prop_iter_end(&it);
  return result;
}

static ant_value_t js_URLSearchParams(ant_params_t) {
  if (is_undefined(call_new_target))
    return js_mkerr_typed(js, JS_ERR_TYPE, "Class constructor URLSearchParams cannot be invoked without 'new'");

  ant_value_t entries = js_mkarr(js);
  ant_value_t obj = usp_new(js, js_mkundef(), entries);
  if (nargs < 1 || is_undefined(args[0]) || is_null(args[0])) return obj;

  ant_value_t init = args[0];
  if (vtype(init) == kTypeString) {
    const char *s = js_getstr(js, init, NULL);
    if (s) js_set_slot(obj, SLOT_ENTRIES, parse_query_string_init(js, s));
    return obj;
  }

  if (!is_object_type(init) && !is_callable(init)) return usp_init_from_primitive(js, obj, init);
  if (usp_is_urlsearchparams(js, init)) return usp_init_from_usp(js, obj, entries, init);

  ant_value_t iter_fn = js_get_sym(js, init, js->sym.iterator_sym);
  if (is_err(iter_fn)) return iter_fn;
  if (Ant_Exception_Pending(js)) return Ant_Exception_Current(js);

  if (is_undefined(iter_fn) || is_null(iter_fn)) return usp_init_from_record(js, obj, entries, init);
  if (!is_callable(iter_fn)) return usp_not_iterable(js);

  return usp_init_from_iterable(js, obj, entries, init);
}

void url_init_search_params(ant_t *js) {
  js->builtins.usp_iter_proto = Ant_Object_CreateBare(js);
  js_set_proto_init(js->builtins.usp_iter_proto, js->sym.iterator_proto);
  js_set(js, js->builtins.usp_iter_proto, "next", Ant_Function_Create(usp_iter_next));
  js_set_descriptor(js, js->builtins.usp_iter_proto, "next", 4, JS_DESC_W | JS_DESC_E | JS_DESC_C);
  js_set_sym(js, js->builtins.usp_iter_proto, js->sym.iterator_sym, Ant_Function_Create(sym_this_cb));

  js->builtins.usp_proto = Ant_Object_CreateBare(js);
  js_set(js, js->builtins.usp_proto, "get",      Ant_Function_CreateWithArity(usp_get, 1));
  js_set(js, js->builtins.usp_proto, "getAll",   Ant_Function_CreateWithArity(usp_getAll, 1));
  js_set(js, js->builtins.usp_proto, "has",      Ant_Function_CreateWithArity(usp_has, 1));
  js_set(js, js->builtins.usp_proto, "set",      Ant_Function_CreateWithArity(usp_set, 2));
  js_set(js, js->builtins.usp_proto, "append",   Ant_Function_CreateWithArity(usp_append, 2));
  js_set(js, js->builtins.usp_proto, "delete",   Ant_Function_CreateWithArity(usp_delete, 1));
  js_set(js, js->builtins.usp_proto, "sort",     Ant_Function_Create(usp_sort));
  js_set(js, js->builtins.usp_proto, "toString", Ant_Function_Create(usp_toString));
  js_set(js, js->builtins.usp_proto, "forEach",  Ant_Function_CreateWithArity(usp_forEach, 1));
  js_set_getter_desc(js, js->builtins.usp_proto, "size", 4, Ant_Function_Create(usp_size_get), JS_DESC_C);

  js_set(js, js->builtins.usp_proto, "entries", Ant_Function_Create(usp_entries_fn));
  js_set(js, js->builtins.usp_proto, "keys",    Ant_Function_Create(usp_keys_fn));
  js_set(js, js->builtins.usp_proto, "values",  Ant_Function_Create(usp_values_fn));
  
  js_set_sym(js, js->builtins.usp_proto, js->sym.iterator_sym, js_get(js, js->builtins.usp_proto, "entries"));
  js_set_tostringtag(js, js->builtins.usp_proto, js_mkstr(js, "URLSearchParams", 15));

  ant_value_t usp_ctor = Ant_Function_CreateIntrinsicConstructor(js, js_URLSearchParams, js->builtins.usp_proto, "URLSearchParams", 0);
  js_set_global_builtin(js, "URLSearchParams", usp_ctor);
}
