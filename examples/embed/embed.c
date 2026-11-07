/**
 * Ant JavaScript Engine - Embedding Example
 * This demonstrates how to embed the Ant JS engine in a C application.
 *
 * to build and run:
 * meson setup build-embed -Db_lto=false
 * meson compile -C build-embed embed
 * ./build-embed/embed-example
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "ant/bootstrap.h"
#include "ant/cfunc.h"
#include "ant/errors.h"
#include "ant/reactor.h"
#include "ant/silver/call.h"
#include "ant/silver/vm.h"

#define SEPARATOR "════════════════════════════════════════════════════════════"
#define SUBSEP    "────────────────────────────────────────────────────────────"

static void print_header(int num, const char *title) {
  printf("\n%s\n", SEPARATOR);
  printf("  Example %d: %s\n", num, title);
  printf("%s\n\n", SUBSEP);
}

static ant_t *create_isolate(void *stack_base) {
  ant_t *js = Ant_Create(nullptr);
  if (!js) {
    fprintf(stderr, "Failed to create JS runtime\n");
    return NULL;
  }

  js_setstackbase(js, stack_base);
  js_setstacklimit(js, os_thread_stack_size() * 3 / 4);

  static char *default_argv[] = { "embed_example", NULL };
  ant_runtime_init(js, 1, default_argv, NULL);
  ant_bootstrap_modules(js);

  return js;
}

static void example_basic_eval(void) {
  print_header(1, "Basic Evaluation");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  const char *code = "1 + 2 * 3";
  ant_value_t result = js_eval_bytecode_eval(js, code, strlen(code), false);

  if (vtype(result) == kTypeNumber) {
    printf("  Result: %g\n", Ant_Value_AsNumberUnchecked(result));
  } else if (vtype(result) == kTypeError) {
    printf("  Error: %s\n", js_str(js, result));
  }

  Ant_Destroy(js);
}

static ant_value_t my_add(ant_params_t) {
  if (!js_chkargs(args, nargs, "dd")) {
    return js_mkerr(js, "add() expects two numbers");
  }

  double a = Ant_Value_AsNumberUnchecked(args[0]);
  double b = Ant_Value_AsNumberUnchecked(args[1]);

  return Ant_Value_FromNumber(a + b);
}

static ant_value_t my_greet(ant_params_t) {
  if (nargs < 1 || vtype(args[0]) != kTypeString) {
    return js_mkerr(js, "greet() expects a string");
  }

  size_t len;
  char *name = js_getstr(js, args[0], &len);

  char buf[256];
  snprintf(buf, sizeof(buf), "Hello, %s!", name);

  return js_mkstr(js, buf, strlen(buf));
}

static ant_value_t my_create_point(ant_params_t) {
  if (!js_chkargs(args, nargs, "dd")) {
    return js_mkerr(js, "createPoint() expects two numbers");
  }

  ant_value_t obj = Ant_Object_CreateBare(js);
  js_set(js, obj, "x", args[0]);
  js_set(js, obj, "y", args[1]);

  return obj;
}

static void example_c_functions(void) {
  print_header(2, "C Functions");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  ant_value_t global = js_glob(js);
  js_set(js, global, "add", Ant_Function_Create(my_add));
  js_set(js, global, "greet", Ant_Function_Create(my_greet));
  js_set(js, global, "createPoint", Ant_Function_Create(my_create_point));

  const char *code1 = "add(10, 32)";
  ant_value_t r1 = js_eval_bytecode_eval(js, code1, strlen(code1), false);
  printf("  add(10, 32) = %g\n", Ant_Value_AsNumberUnchecked(r1));

  const char *code2 = "greet('World')";
  ant_value_t r2 = js_eval_bytecode_eval(js, code2, strlen(code2), false);
  printf("  greet('World') = %s\n", js_str(js, r2));

  const char *code3 = "let p = createPoint(3, 4); p.x * p.x + p.y * p.y";
  ant_value_t r3 = js_eval_bytecode_eval(js, code3, strlen(code3), false);
  printf("  distance² = %g\n", Ant_Value_AsNumberUnchecked(r3));

  Ant_Destroy(js);
}

static void example_objects_arrays(void) {
  print_header(3, "Objects and Arrays");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  ant_value_t global = js_glob(js);
  ant_value_t config = Ant_Object_CreateBare(js);
  
  js_set(js, config, "debug", Ant_Value_True);
  js_set(js, config, "version", Ant_Value_FromNumber(1.0));
  js_set(js, config, "name", js_mkstr(js, "MyApp", 5));
  js_set(js, global, "config", config);

  ant_value_t arr = Ant_Array_Create(js);
  js_arr_push(js, arr, Ant_Value_FromNumber(10));
  js_arr_push(js, arr, Ant_Value_FromNumber(20));
  js_arr_push(js, arr, Ant_Value_FromNumber(30));
  js_set(js, global, "numbers", arr);

  const char *code = "config.name + ' v' + config.version + ' - sum: ' + numbers.reduce((a,b) => a+b, 0)";
  ant_value_t result = js_eval_bytecode_eval(js, code, strlen(code), false);
  printf("  Result: %s\n", js_str(js, result));

  ant_value_t name_val = js_get(js, config, "name");
  printf("  config.name: %s\n", js_str(js, name_val));
  
  ant_value_t debug_val = js_get(js, config, "debug");
  printf("  config.debug: %s\n", js_str(js, debug_val));

  Ant_Destroy(js);
}

static void example_error_handling(void) {
  print_header(4, "Error Handling");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  const char *bad_code = "let x = {";
  ant_value_t r1 = js_eval_bytecode_eval(js, bad_code, strlen(bad_code), false);
  if (vtype(r1) == kTypeError) {
    printf("  Syntax error:    %s\n", js_str(js, r1));
    Ant_Exception_Clear(js);
  }

  const char *ref_err = "undefinedVariable + 1";
  ant_value_t r2 = js_eval_bytecode_eval(js, ref_err, strlen(ref_err), false);
  if (vtype(r2) == kTypeError) {
    printf("  Reference error: %s\n", js_str(js, r2));
    Ant_Exception_Clear(js);
  }

  ant_value_t global = js_glob(js);
  js_set(js, global, "add", Ant_Function_Create(my_add));

  const char *type_err = "add('not', 'numbers')";
  ant_value_t r3 = js_eval_bytecode_eval(js, type_err, strlen(type_err), false);
  if (vtype(r3) == kTypeError) {
    printf("  Type error:      %s\n", js_str(js, r3));
    Ant_Exception_Clear(js);
  }

  Ant_Destroy(js);
}

static void example_call_js_from_c(void) {
  print_header(5, "Calling JS from C");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  const char *code =
    "export function multiply(a, b) {"
    "    return a * b;"
    "}"
    "export function formatName(first, last) {"
    "    return last + ', ' + first;"
    "}";

  ant_value_t ns = Ant_Object_CreateBare(js);
  
  ant_value_t eval_result = js_esm_eval_module_source(
    js, "embed_call_js_from_c.mjs", 
    code, strlen(code), ns
  );
  
  if (vtype(eval_result) == kTypeError) {
    printf("  Error: %s\n", js_str(js, eval_result));
    Ant_Destroy(js);
    return;
  }

  ant_value_t multiply_fn = js_get(js, ns, "multiply");
  ant_value_t format_fn = js_get(js, ns, "formatName");

  ant_value_t args1[] = { Ant_Value_FromNumber(6), Ant_Value_FromNumber(7) };
  ant_value_t result1 = sv_vm_call(js->vm, js, multiply_fn, Ant_Value_Undefined, args1, 2, NULL, Ant_Value_Undefined);
  printf("  multiply(6, 7) = %g\n", Ant_Value_AsNumberUnchecked(result1));

  ant_value_t args2[] = {
    js_mkstr(js, "John", 4),
    js_mkstr(js, "Doe", 3)
  };

  ant_value_t result2 = sv_vm_call(js->vm, js, format_fn, Ant_Value_Undefined, args2, 2, NULL, Ant_Value_Undefined);
  printf("  formatName('John', 'Doe') = %s\n", js_str(js, result2));

  Ant_Destroy(js);
}

static void example_iterate_properties(void) {
  print_header(6, "Iterating Properties");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  const char *code = "({ name: 'Alice', age: 30, city: 'NYC' })";
  ant_value_t obj = js_eval_bytecode_eval(js, code, strlen(code), false);

  ant_iter_t iter = js_prop_iter_begin(js, obj);
  const char *key;
  size_t key_len;
  ant_value_t value;

  printf("  Object properties:\n");
  while (js_prop_iter_next(&iter, &key, &key_len, &value)) {
    printf("    • %.*s = %s\n", (int)key_len, key, js_str(js, value));
  }
  js_prop_iter_end(&iter);

  Ant_Destroy(js);
}

static ant_value_t method_get_full_name(ant_params_t) {
  ant_value_t first = js_get(js, call_this, "firstName");
  ant_value_t last = js_get(js, call_this, "lastName");

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
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  ant_value_t person = Ant_Object_CreateBare(js);
  js_set(js, person, "firstName", js_mkstr(js, "Jane", 4));
  js_set(js, person, "lastName", js_mkstr(js, "Smith", 5));
  js_set(js, person, "getFullName", Ant_Function_Create(method_get_full_name));

  js_set(js, js_glob(js), "person", person);

  const char *code = "person.getFullName()";
  ant_value_t result = js_eval_bytecode_eval(js, code, strlen(code), false);
  printf("  person.getFullName() = %s\n", js_str(js, result));

  Ant_Destroy(js);
}

static void example_stateful_session(void) {
  print_header(8, "Stateful Session");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  const char *code =
    "let counter = 0;"
    "export function increment() { return ++counter; }"
    "export function getCount() { return counter; }";

  ant_value_t ns = Ant_Object_CreateBare(js);
  
  ant_value_t eval_result = js_esm_eval_module_source(
    js, "embed_stateful_session.mjs", 
    code, strlen(code), ns
  );
  
  if (vtype(eval_result) == kTypeError) {
    printf("  Error: %s\n", js_str(js, eval_result));
    Ant_Destroy(js);
    return;
  }

  ant_value_t increment = js_get(js, ns, "increment");
  for (int i = 0; i < 3; i++) {
    ant_value_t call_result = sv_vm_call(js->vm, js, increment, Ant_Value_Undefined, NULL, 0, NULL, Ant_Value_Undefined);
    if (vtype(call_result) == kTypeError) {
      printf("  Error in increment %d: %s\n", i + 1, js_str(js, call_result));
      Ant_Destroy(js);
      return;
    }
  }

  ant_value_t get_count = js_get(js, ns, "getCount");
  ant_value_t result = sv_vm_call(js->vm, js, get_count, Ant_Value_Undefined, NULL, 0, NULL, Ant_Value_Undefined);
  printf("  Final count: %g\n", Ant_Value_AsNumberUnchecked(result));

  Ant_Destroy(js);
}

static void example_async_event_loop(void) {
  print_header(9, "Async & Event Loop");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  const char *code =
    "export let results = [];"
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

  ant_value_t ns = Ant_Object_CreateBare(js);
  ant_value_t result = js_esm_eval_module_source(
    js, "embed_async_event_loop.mjs", 
    code, strlen(code), ns
  );
  
  if (vtype(result) == kTypeError) {
    printf("  Error: %s\n", js_str(js, result));
    Ant_Destroy(js);
    return;
  }

  js_run_event_loop(js);
  ant_value_t results = js_get(js, ns, "results");

  printf("  Execution order:\n");
  ant_offset_t len = js_arr_len(js, results);
  for (ant_offset_t i = 0; i < len; i++) {
    ant_value_t item = js_arr_get(js, results, i);
    printf("    %llu. %s\n", (unsigned long long)i + 1, js_str(js, item));
  }

  Ant_Destroy(js);
}

static void example_console_logging(void) {
  print_header(10, "Console Logging");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  const char *code =
    "console.log('Hello from JavaScript!');"
    "console.log('Number:', 42, 'Boolean:', true);"
    "console.log('Object:', { name: 'test', value: 123 });"
    "console.log('Array:', [1, 2, 3]);"
    "console.warn('This is a warning');"
    "console.error('This is an error');"
    "'done'";

  ant_value_t result = js_eval_bytecode_eval(js, code, strlen(code), false);
  if (vtype(result) == kTypeError) {
    printf("  Error: %s\n", js_str(js, result));
  }

  Ant_Destroy(js);
}

static void example_global_this(void) {
  print_header(11, "GlobalThis");

  volatile char stack_base;
  ant_t *js = create_isolate((void *)&stack_base);
  if (!js) return;

  ant_value_t global = js_glob(js);
  js_set(js, global, "myNumber", Ant_Value_FromNumber(42));
  js_set(js, global, "myString", js_mkstr(js, "hello from C", 12));
  js_set(js, global, "myBool", Ant_Value_True);

  ant_value_t myObj = Ant_Object_CreateBare(js);
  js_set(js, myObj, "a", Ant_Value_FromNumber(1));
  js_set(js, myObj, "b", Ant_Value_FromNumber(2));
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
    "console.log('  addedFromJS:', addedFromJS);";

  ant_value_t result = js_eval_bytecode_eval(js, code, strlen(code), false);
  if (vtype(result) == kTypeError) {
    printf("  Error: %s\n", js_str(js, result));
  }

  ant_value_t added = js_get(js, global, "addedFromJS");
  printf("\n  Read from C: addedFromJS = %s\n", js_str(js, added));

  Ant_Destroy(js);
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
