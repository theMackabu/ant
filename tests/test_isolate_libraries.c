#include "internal.h"
#include "gc.h"
#include "gc/roots.h"
#include "esm/library.h"
#include "esm/loader.h"
#include "bootstrap.h"
#include "runtime.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ant_value_t foreign_ns;
static bool saw_foreign;

static void CheckRoot(ant_t *js, ant_value_t value) {
  (void)js;
  if (value == foreign_ns) saw_foreign = true;
}

static char *argv0[] = { "test-isolate-libraries", NULL };

static ant_t *Create(void) {
  ant_t *js = Ant_Create(nullptr);
  assert(js);
  js_setstackbase(js, NULL);
  ant_runtime_init(js, 1, argv0, NULL);
  ant_bootstrap_modules(js);
  assert(!Ant_Exception_Pending(js));
  return js;
}

static const ant_library_entry_t *Find(void) {
  const char *name = "node:string_decoder";
  ant_builtin_name_kind_t kind = ANT_BUILTIN_NAME_NONE;
  const ant_library_entry_t *lib = ant_library_find(name, strlen(name), &kind);
  assert(lib && kind == ANT_BUILTIN_NAME_NODE);
  assert(ant_library_find("string_decoder", 14, NULL) == lib);
  assert(ant_library_find("ant:string_decoder", 18, NULL) == lib);
  assert(!ant_library_find("node:ffi", 8, NULL));
  return lib;
}

static ant_value_t Load(ant_t *js) {
  ant_value_t ns = ant_library_load(js, Find());
  assert(is_object_type(ns));
  assert(ant_library_load(js, Find()) == ns);
  ant_value_t esm = ant_library_namespace(js, Find());
  assert(is_object_type(esm));
  assert(ant_library_namespace(js, Find()) == esm);
  return ns;
}

static int host_inits;

static ant_value_t HostLibrary(ant_t *js) {
  host_inits++;
  ant_value_t lib = Ant_Object_Create(js);
  js_set(js, lib, "answer", Ant_Value_FromNumber(42));
  return lib;
}

static const char host_source[] =
  "import native from 'ant:internal/host-test';\n"
  "export const doubled = native.answer * 2;\n"
  "export default native;\n";

static ant_value_t Import(ant_t *js, const char *specifier) {
  ant_value_t ns = js_esm_import_sync_cstr(js, specifier, strlen(specifier));
  assert(!is_err(ns) && is_object_type(ns));
  return ns;
}

static void HostModules(void) {
  ant_t *js = Create();
  ant_t *other = Create();

  assert(!Ant_Module_Register(js, "node:fs", HostLibrary));
  assert(!Ant_Module_Register(js, "ant:ffi", HostLibrary));
  assert(Ant_Module_Register(js, "ant:internal/host-test", HostLibrary));
  assert(!Ant_Module_Register(js, "ant:internal/host-test", HostLibrary));
  assert(Ant_Module_RegisterSource(js, "ant:host-test", host_source, sizeof(host_source) - 1));
  assert(host_inits == 0);

  ant_value_t ns = Import(js, "ant:host-test");
  assert(Ant_Value_AsNumberUnchecked(js_get(js, ns, "doubled")) == 84);
  assert(Ant_Value_AsNumberUnchecked(js_get(js, js_get(js, ns, "default"), "answer")) == 42);
  assert(Import(js, "ant:host-test") == ns);
  assert(host_inits == 1);

  ant_value_t native = Import(js, "ant:internal/host-test");
  gc_run(js);
  gc_run(js);
  assert(Import(js, "ant:internal/host-test") == native);
  assert(host_inits == 1);

  assert(is_err(js_esm_import_sync_cstr(other, "ant:host-test", 13)));
  assert(!js_esm_is_node_builtin("ant:host-test", 13));

  Ant_Destroy(other);
  Ant_Destroy(js);
  puts("PASS host modules are registered per isolate");
}

int main(void) {
  HostModules();

  for (int destroy_first = 0; destroy_first < 2; destroy_first++) {
    ant_t *first = Create();
    ant_t *second = Create();

    ant_value_t a = Load(first);
    ant_value_t b = Load(second);
    assert(a != b);

    foreign_ns = b;
    saw_foreign = false;
    gc_visit_roots(first, CheckRoot);
    assert(!saw_foreign);

    ant_t *survivor = destroy_first ? second : first;
    ant_value_t kept = destroy_first ? b : a;
    Ant_Destroy(destroy_first ? first : second);
    gc_run(survivor);
    gc_run(survivor);
    assert(Load(survivor) == kept);
    Ant_Destroy(survivor);
  }

  puts("PASS registered library namespaces are per isolate");
}
