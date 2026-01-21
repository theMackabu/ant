/**
 * Ant JavaScript Engine - Embedding Example
 * This demonstrates how to embed the Ant JS engine in a C application.
 *
 * meson setup build -Dbuild_examples=true
 * meson compile -C build
 *
 * ./build/embed_example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ant.h>

#define SEPARATOR "════════════════════════════════════════════════════════════"
#define SUBSEP    "────────────────────────────────────────────────────────────"

static void print_header(int num, const char *title) {
  printf("\n%s\n", SEPARATOR);
  printf("  Example %d: %s\n", num, title);
  printf("%s\n\n", SUBSEP);
}

static void example_basic_eval(void) {
  print_header(1, "Basic Evaluation");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) {
    fprintf(stderr, "Failed to create JS runtime\n");
    return;
  }

  js_mkscope(js);

  const char *code = "1 + 2 * 3";
  jsval_t result = js_eval(js, code, strlen(code));

  if (js_type(result) == JS_NUM) {
    printf("  Result: %g\n", js_getnum(result));
  } else if (js_type(result) == JS_ERR) {
    printf("  Error: %s\n", js_str(js, result));
  }

  js_destroy(js);
}

static jsval_t my_add(struct js *js, jsval_t *args, int nargs) {
  if (!js_chkargs(args, nargs, "dd")) {
    return js_mkerr(js, "add() expects two numbers");
  }

  double a = js_getnum(args[0]);
  double b = js_getnum(args[1]);

  return js_mknum(a + b);
}

static jsval_t my_greet(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1 || js_type(args[0]) != JS_STR) {
    return js_mkerr(js, "greet() expects a string");
  }

  size_t len;
  char *name = js_getstr(js, args[0], &len);

  char buf[256];
  snprintf(buf, sizeof(buf), "Hello, %s!", name);

  return js_mkstr(js, buf, strlen(buf));
}

static jsval_t my_create_point(struct js *js, jsval_t *args, int nargs) {
  if (!js_chkargs(args, nargs, "dd")) {
    return js_mkerr(js, "createPoint() expects two numbers");
  }

  jsval_t obj = js_mkobj(js);
  js_set(js, obj, "x", args[0]);
  js_set(js, obj, "y", args[1]);

  return obj;
}

static void example_c_functions(void) {
  print_header(2, "C Functions");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) return;

  js_mkscope(js);

  jsval_t global = js_glob(js);
  js_set(js, global, "add", js_mkfun(my_add));
  js_set(js, global, "greet", js_mkfun(my_greet));
  js_set(js, global, "createPoint", js_mkfun(my_create_point));

  const char *code1 = "add(10, 32)";
  jsval_t r1 = js_eval(js, code1, strlen(code1));
  printf("  add(10, 32) = %g\n", js_getnum(r1));

  const char *code2 = "greet('World')";
  jsval_t r2 = js_eval(js, code2, strlen(code2));
  printf("  greet('World') = %s\n", js_str(js, r2));

  const char *code3 = "let p = createPoint(3, 4); p.x * p.x + p.y * p.y";
  jsval_t r3 = js_eval(js, code3, strlen(code3));
  printf("  distance² = %g\n", js_getnum(r3));

  js_destroy(js);
}

static void example_objects_arrays(void) {
  print_header(3, "Objects and Arrays");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) return;

  js_mkscope(js);
  jsval_t global = js_glob(js);

  jsval_t config = js_mkobj(js);
  js_set(js, config, "debug", js_mktrue());
  js_set(js, config, "version", js_mknum(1.0));
  js_set(js, config, "name", js_mkstr(js, "MyApp", 5));
  js_set(js, global, "config", config);

  jsval_t arr = js_mkarr(js);
  js_arr_push(js, arr, js_mknum(10));
  js_arr_push(js, arr, js_mknum(20));
  js_arr_push(js, arr, js_mknum(30));
  js_set(js, global, "numbers", arr);

  const char *code = "config.name + ' v' + config.version + ' - sum: ' + numbers.reduce((a,b) => a+b, 0)";
  jsval_t result = js_eval(js, code, strlen(code));
  printf("  Result: %s\n", js_str(js, result));

  jsval_t name_val = js_get(js, config, "name");
  printf("  config.name from C: %s\n", js_str(js, name_val));

  js_destroy(js);
}

static void example_error_handling(void) {
  print_header(4, "Error Handling");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) return;

  js_mkscope(js);

  const char *bad_code = "let x = {";
  jsval_t r1 = js_eval(js, bad_code, strlen(bad_code));
  if (js_type(r1) == JS_ERR) {
    printf("  Syntax error:    %s\n", js_str(js, r1));
  }

  const char *ref_err = "undefinedVariable + 1";
  jsval_t r2 = js_eval(js, ref_err, strlen(ref_err));
  if (js_type(r2) == JS_ERR) {
    printf("  Reference error: %s\n", js_str(js, r2));
  }

  jsval_t global = js_glob(js);
  js_set(js, global, "add", js_mkfun(my_add));

  const char *type_err = "add('not', 'numbers')";
  jsval_t r3 = js_eval(js, type_err, strlen(type_err));
  if (js_type(r3) == JS_ERR) {
    printf("  Type error:      %s\n", js_str(js, r3));
  }

  js_destroy(js);
}

static void example_call_js_from_c(void) {
  print_header(5, "Calling JS from C");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) return;

  jsval_t scope = js_mkscope(js);

  const char *code =
    "function multiply(a, b) {"
    "    return a * b;"
    "}"

    "function formatName(first, last) {"
    "    return last + ', ' + first;"
    "}";

  js_eval(js, code, strlen(code));

  jsval_t multiply_fn = js_get(js, scope, "multiply");
  jsval_t format_fn = js_get(js, scope, "formatName");

  jsval_t args1[] = { js_mknum(6), js_mknum(7) };
  jsval_t result1 = js_call(js, multiply_fn, args1, 2);
  printf("  multiply(6, 7) = %g\n", js_getnum(result1));

  jsval_t args2[] = {
    js_mkstr(js, "John", 4),
    js_mkstr(js, "Doe", 3)
  };

  jsval_t result2 = js_call(js, format_fn, args2, 2);
  printf("  formatName('John', 'Doe') = %s\n", js_str(js, result2));

  js_destroy(js);
}

static void example_iterate_properties(void) {
  print_header(6, "Iterating Properties");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) return;

  js_mkscope(js);

  const char *code = "({ name: 'Alice', age: 30, city: 'NYC' })";
  jsval_t obj = js_eval(js, code, strlen(code));

  js_prop_iter_t iter = js_prop_iter_begin(js, obj);
  const char *key;
  size_t key_len;
  jsval_t value;

  printf("  Object properties:\n");
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    printf("    • %.*s = %s\n", (int)key_len, key, js_str(js, value));
  }
  js_prop_iter_end(&iter);

  js_destroy(js);
}

static jsval_t method_get_full_name(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;

  jsval_t this_obj = js_getthis(js);

  jsval_t first = js_get(js, this_obj, "firstName");
  jsval_t last = js_get(js, this_obj, "lastName");

  size_t first_len, last_len;
  char *first_str = js_getstr(js, first, &first_len);
  char *last_str = js_getstr(js, last, &last_len);

  char buf[256];
  snprintf(buf, sizeof(buf), "%s %s", first_str, last_str);

  return js_mkstr(js, buf, strlen(buf));
}

static void example_this_context(void) {
  print_header(7, "'this' Context");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) return;

  js_mkscope(js);

  jsval_t person = js_mkobj(js);
  js_set(js, person, "firstName", js_mkstr(js, "Jane", 4));
  js_set(js, person, "lastName", js_mkstr(js, "Smith", 5));
  js_set(js, person, "getFullName", js_mkfun(method_get_full_name));

  js_set(js, js_glob(js), "person", person);

  const char *code = "person.getFullName()";
  jsval_t result = js_eval(js, code, strlen(code));
  printf("  person.getFullName() = %s\n", js_str(js, result));

  js_destroy(js);
}

static void example_stateful_session(void) {
  print_header(8, "Stateful Session");

  struct js *js = js_create_dynamic(0, 0);
  if (!js) return;

  js_mkscope(js);

  const char *scripts[] = {
    "let counter = 0;",
    "function increment() { return ++counter; }",
    "function getCount() { return counter; }",
    "increment(); increment(); increment();",
    "getCount()"
  };

  jsval_t result = js_mkundef();
  for (int i = 0; i < 5; i++) {
    result = js_eval(js, scripts[i], strlen(scripts[i]));
    if (js_type(result) == JS_ERR) {
      printf("  Error in script %d: %s\n", i, js_str(js, result));
      break;
    }
  }

  printf("  Final count: %g\n", js_getnum(result));

  js_destroy(js);
}


int main(void) {
  printf("\n%s\n", SEPARATOR);
  printf("     Ant JavaScript Engine - Embedding Examples\n");
  printf("%s\n", SEPARATOR);

  example_basic_eval();
  example_c_functions();
  example_objects_arrays();
  example_error_handling();
  example_call_js_from_c();
  example_iterate_properties();
  example_this_context();
  example_stateful_session();

  return EXIT_SUCCESS;
}