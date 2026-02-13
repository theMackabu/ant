/**
 * Ant JavaScript Engine - Embedding Example
 * This demonstrates how to embed the Ant JS engine in a C application.
 *
 * to build:
 * ./libant/build.sh  
 * ./libant/example.sh  
 *
 * to run:
 * ./libant/dist/embed
 */

#include <ant.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEPARATOR "════════════════════════════════════════════════════════════"
#define SUBSEP    "────────────────────────────────────────────────────────────"

static void print_header(int num, const char *title) {
  printf("\n%s\n", SEPARATOR);
  printf("  Example %d: %s\n", num, title);
  printf("%s\n\n", SUBSEP);
}

static struct js *create_js_runtime(void *stack_base) {
  struct js *js = js_create_dynamic();
  if (!js) {
    fprintf(stderr, "Failed to create JS runtime\n");
    return NULL;
  }

  js_setstackbase(js, stack_base);

  static char *default_argv[] = { "embed_example", NULL };
  ant_runtime_init(js, 1, default_argv, NULL);

  return js;
}

static void example_basic_eval(void) {
  print_header(1, "Basic Evaluation");

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
  if (!js) return;

  js_mkscope(js);

  const char *code = "1 + 2 * 3";
  jsval_t result = js_eval(js, code, strlen(code));

  if (vtype(result) == T_NUM) {
    printf("  Result: %g\n", js_getnum(result));
  } else if (vtype(result) == T_ERR) {
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
  if (nargs < 1 || vtype(args[0]) != T_STR) {
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

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
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

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
  if (!js) return;

  js_mkscope(js);
  jsval_t global = js_glob(js);

  jsval_t config = js_mkobj(js);
  js_set(js, config, "debug", js_true);
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
  printf("  config.name: %s\n", js_str(js, name_val));
  
  jsval_t debug_val = js_get(js, config, "debug");
  printf("  config.debug: %s\n", js_str(js, debug_val));

  js_destroy(js);
}

static void example_error_handling(void) {
  print_header(4, "Error Handling");

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
  if (!js) return;

  js_mkscope(js);

  const char *bad_code = "let x = {";
  jsval_t r1 = js_eval(js, bad_code, strlen(bad_code));
  if (vtype(r1) == T_ERR) {
    printf("  Syntax error:    %s\n", js_str(js, r1));
  }

  const char *ref_err = "undefinedVariable + 1";
  jsval_t r2 = js_eval(js, ref_err, strlen(ref_err));
  if (vtype(r2) == T_ERR) {
    printf("  Reference error: %s\n", js_str(js, r2));
  }

  jsval_t global = js_glob(js);
  js_set(js, global, "add", js_mkfun(my_add));

  const char *type_err = "add('not', 'numbers')";
  jsval_t r3 = js_eval(js, type_err, strlen(type_err));
  if (vtype(r3) == T_ERR) {
    printf("  Type error:      %s\n", js_str(js, r3));
  }

  js_destroy(js);
}

static void example_call_js_from_c(void) {
  print_header(5, "Calling JS from C");

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
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

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
  if (!js) return;

  js_mkscope(js);

  const char *code = "({ name: 'Alice', age: 30, city: 'NYC' })";
  jsval_t obj = js_eval(js, code, strlen(code));

  ant_iter_t iter = js_prop_iter_begin(js, obj);
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

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
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

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
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
    if (vtype(result) == T_ERR) {
      printf("  Error in script %d: %s\n", i, js_str(js, result));
      break;
    }
  }

  printf("  Final count: %g\n", js_getnum(result));

  js_destroy(js);
}

static void example_async_event_loop(void) {
  print_header(9, "Async & Event Loop");

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
  if (!js) return;

  init_symbol_module();
  init_builtin_module();
  init_timer_module();

  jsval_t scope = js_mkscope(js);

  const char *code =
    "let results = [];"
    ""
    "setTimeout(() => {"
    "  results.push('timer 1 (50ms)');"
    "}, 50);"
    ""
    "setTimeout(() => {"
    "  results.push('timer 2 (10ms)');"
    "}, 10);"
    ""
    "Promise.resolve('promise 1').then(v => {"
    "  results.push(v);"
    "});"
    ""
    "queueMicrotask(() => {"
    "  results.push('microtask');"
    "});"
    ""
    "results.push('sync');";

  jsval_t result = js_eval(js, code, strlen(code));
  if (vtype(result) == T_ERR) {
    printf("  Error: %s\n", js_str(js, result));
    js_destroy(js);
    return;
  }

  js_run_event_loop(js);

  jsval_t results = js_get(js, scope, "results");

  printf("  Execution order:\n");
  jsoff_t len = js_arr_len(js, results);
  for (jsoff_t i = 0; i < len; i++) {
    jsval_t item = js_arr_get(js, results, i);
    printf("    %llu. %s\n", (unsigned long long)i + 1, js_str(js, item));
  }

  js_destroy(js);
}

static void example_console_logging(void) {
  print_header(10, "Console Logging");

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
  if (!js) return;

  init_console_module();
  js_mkscope(js);

  const char *code =
    "console.log('Hello from JavaScript!');"
    "console.log('Number:', 42, 'Boolean:', true);"
    "console.log('Object:', { name: 'test', value: 123 });"
    "console.log('Array:', [1, 2, 3]);"
    "console.warn('This is a warning');"
    "console.error('This is an error');"
    "'done'";

  jsval_t result = js_eval(js, code, strlen(code));
  if (vtype(result) == T_ERR) {
    printf("  Error: %s\n", js_str(js, result));
  }

  js_destroy(js);
}

static void example_global_this(void) {
  print_header(11, "GlobalThis");

  volatile char stack_base;
  struct js *js = create_js_runtime((void *)&stack_base);
  if (!js) return;

  init_console_module();
  js_mkscope(js);

  jsval_t global = js_glob(js);
  js_set(js, global, "myNumber", js_mknum(42));
  js_set(js, global, "myString", js_mkstr(js, "hello from C", 12));
  js_set(js, global, "myBool", js_true);

  jsval_t myObj = js_mkobj(js);
  js_set(js, myObj, "a", js_mknum(1));
  js_set(js, myObj, "b", js_mknum(2));
  js_set(js, global, "myObject", myObj);

  const char *code =
    "console.log('globalThis.myNumber:', globalThis.myNumber);"
    "console.log('globalThis.myString:', globalThis.myString);"
    "console.log('globalThis.myBool:', globalThis.myBool);"
    "console.log('globalThis.myObject:', globalThis.myObject);"
    ""
    "globalThis.addedFromJS = 'I was added from JavaScript';"
    "console.log('globalThis.addedFromJS:', globalThis.addedFromJS);"
    ""
    "console.log('\\nAll custom globals:');"
    "console.log('  myNumber:', myNumber);"
    "console.log('  myString:', myString);"
    "console.log('  myBool:', myBool);"
    "console.log('  myObject:', myObject);"
    "console.log('  addedFromJS:', addedFromJS);"
    "console.log(this)";

  jsval_t result = js_eval(js, code, strlen(code));
  if (vtype(result) == T_ERR) {
    printf("  Error: %s\n", js_str(js, result));
  }

  jsval_t added = js_get(js, global, "addedFromJS");
  printf("\n  Read from C: addedFromJS = %s\n", js_str(js, added));

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
  example_async_event_loop();
  example_console_logging();
  example_global_this();

  return EXIT_SUCCESS;
}