#include <stdlib.h>
#include <string.h>
#include <yyjson.h>

#include "ant.h"
#include "runtime.h"
#include "modules/json.h"

static jsval_t yyjson_to_jsval(struct js *js, yyjson_val *val) {
  if (!val) return js_mkundef();
  
  yyjson_type type = yyjson_get_type(val);
  
  switch (type) {
    case YYJSON_TYPE_NULL:
      return js_mknull();
    
    case YYJSON_TYPE_BOOL:
      return yyjson_get_bool(val) ? js_mktrue() : js_mkfalse();
    
    case YYJSON_TYPE_NUM: {
      if (yyjson_is_int(val)) {
        return js_mknum((double)yyjson_get_int(val));
      } else if (yyjson_is_uint(val)) {
        return js_mknum((double)yyjson_get_uint(val));
      } else {
        return js_mknum(yyjson_get_real(val));
      }
    }
    
    case YYJSON_TYPE_STR: {
      const char *str = yyjson_get_str(val);
      size_t len = yyjson_get_len(val);
      return js_mkstr(js, str, len);
    }
    
    case YYJSON_TYPE_ARR: {
      jsval_t arr = js_mkobj(js);
      size_t idx, max;
      yyjson_val *item;
      
      yyjson_arr_foreach(val, idx, max, item) {
        char idxstr[32];
        snprintf(idxstr, sizeof(idxstr), "%zu", idx);
        jsval_t value = yyjson_to_jsval(js, item);
        js_set(js, arr, idxstr, value);
      }
      
      js_set(js, arr, "length", js_mknum((double)yyjson_arr_size(val)));
      return arr;
    }
    
    case YYJSON_TYPE_OBJ: {
      jsval_t obj = js_mkobj(js);
      size_t idx, max;
      yyjson_val *key, *item;
      
      yyjson_obj_foreach(val, idx, max, key, item) {
        const char *key_str = yyjson_get_str(key);
        jsval_t value = yyjson_to_jsval(js, item);
        js_set(js, obj, key_str, value);
      }
      
      return obj;
    }
    
    default:
      return js_mkundef();
  }
}

static yyjson_mut_val *jsval_to_yyjson(struct js *js, yyjson_mut_doc *doc, jsval_t val) {
  int type = js_type(val);
  
  switch (type) {
    case JS_UNDEF:
      return yyjson_mut_null(doc);
    
    case JS_NULL:
      return yyjson_mut_null(doc);
    
    case JS_TRUE:
      return yyjson_mut_bool(doc, true);
    
    case JS_FALSE:
      return yyjson_mut_bool(doc, false);
    
    case JS_NUM: {
      double num = js_getnum(val);
      if (num == (int64_t)num) return yyjson_mut_sint(doc, (int64_t)num);
      return yyjson_mut_real(doc, num);
    }
    
    case JS_STR: {
      size_t len;
      char *str = js_getstr(js, val, &len);
      return yyjson_mut_strncpy(doc, str, len);
    }
    
    case JS_OBJ: {
      jsval_t length_val = js_get(js, val, "length");
      
      if (js_type(length_val) == JS_NUM) {
        yyjson_mut_val *arr = yyjson_mut_arr(doc);
        int length = (int)js_getnum(length_val);
        
        for (int i = 0; i < length; i++) {
          char idxstr[32];
          snprintf(idxstr, sizeof(idxstr), "%d", i);
          jsval_t item = js_get(js, val, idxstr);
          yyjson_mut_val *json_item = jsval_to_yyjson(js, doc, item);
          yyjson_mut_arr_add_val(arr, json_item);
        }
        
        return arr;
      } else {
        yyjson_mut_val *obj = yyjson_mut_obj(doc);
        
        js_prop_iter_t iter = js_prop_iter_begin(js, val);
        const char *key;
        size_t key_len;
        jsval_t prop_value;
        
        while (js_prop_iter_next(&iter, &key, &key_len, &prop_value)) {
          if (key_len > 2 && key[0] == '_' && key[1] == '_') continue;
          if (js_type(prop_value) == JS_OBJ) {
            jsval_t code = js_get(js, prop_value, "__code");
            if (js_type(code) == JS_STR) continue;
          }
          
          yyjson_mut_val *json_key = yyjson_mut_strncpy(doc, key, key_len);
          yyjson_mut_val *json_value = jsval_to_yyjson(js, doc, prop_value);
          yyjson_mut_obj_add(obj, json_key, json_value);
        }
        
        js_prop_iter_end(&iter);
        return obj;
      }
    }
    
    default:
      return yyjson_mut_null(doc);
  }
}

jsval_t js_json_parse(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "JSON.parse() requires at least 1 argument");
  }
  
  if (js_type(args[0]) != JS_STR) {
    return js_mkerr(js, "JSON.parse() argument must be a string");
  }
  
  size_t len;
  char *json_str = js_getstr(js, args[0], &len);
  
  yyjson_doc *doc = yyjson_read(json_str, len, 0);
  if (!doc) {
    return js_mkerr(js, "JSON.parse() failed: invalid JSON");
  }
  
  yyjson_val *root = yyjson_doc_get_root(doc);
  jsval_t result = yyjson_to_jsval(js, root);
  
  yyjson_doc_free(doc);
  
  return result;
}

jsval_t js_json_stringify(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr(js, "JSON.stringify() requires at least 1 argument");
  }
  
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) {
    return js_mkerr(js, "JSON.stringify() failed: out of memory");
  }
  
  yyjson_mut_val *root = jsval_to_yyjson(js, doc, args[0]);
  yyjson_mut_doc_set_root(doc, root);
  
  size_t len;
  yyjson_write_flag flg = 0;
  if (nargs >= 3 && js_type(args[2]) != JS_UNDEF && js_type(args[2]) != JS_NULL) {
    int indent = 4;
    if (js_type(args[2]) == JS_NUM) {
      indent = (int)js_getnum(args[2]);
      if (indent < 0) indent = 0;
      if (indent > 10) indent = 10;
    }
    if (indent == 2) {
      flg = YYJSON_WRITE_PRETTY_TWO_SPACES;
    } else if (indent > 0) {
      flg = YYJSON_WRITE_PRETTY;
    }
  }
  
  char *json_str = yyjson_mut_write(doc, flg, &len);
  if (!json_str) {
    yyjson_mut_doc_free(doc);
    return js_mkerr(js, "JSON.stringify() failed: write error");
  }
  
  jsval_t result = js_mkstr(js, json_str, len);
  
  free(json_str);
  yyjson_mut_doc_free(doc);
  
  return result;
}

void init_json_module() {
  struct js *js = rt->js;
  jsval_t json_obj = js_mkobj(js);
  
  js_set(js, json_obj, "parse", js_mkfun(js_json_parse));
  js_set(js, json_obj, "stringify", js_mkfun(js_json_stringify));
  
  js_set(js, json_obj, "@@toStringTag", js_mkstr(js, "JSON", 4));
  js_set(js, js_glob(js), "JSON", json_obj);
}