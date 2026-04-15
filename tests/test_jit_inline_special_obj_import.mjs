function getImportBinding() {
  return import;
}

export async function verifyImportBinding(assert, assertEq) {
  let importBinding = null;
  for (let i = 0; i < 600; i++) {
    importBinding = getImportBinding();
  }

  assert(typeof importBinding === "function", "hot inline helper returns import binding");

  const imported = await importBinding("./test_jit_inline_special_obj_dep.mjs");
  assertEq(imported.marker, "inline-special-obj", "returned import binding loads sibling module");
}
