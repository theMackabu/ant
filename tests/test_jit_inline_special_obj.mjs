function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function assertEq(actual, expected, message) {
  if (actual !== expected) {
    throw new Error(`${message}: expected ${expected}, got ${actual}`);
  }
}

function runNewTargetHot() {
  function readNewTarget() {
    return new.target;
  }

  let last = null;
  for (let i = 0; i < 600; i++) {
    last = readNewTarget();
  }
  return last;
}

class BaseBox {
  static value = 7;
}

class DerivedBox extends BaseBox {
  static readSuperValue() {
    return super.value;
  }
}

const superReader = DerivedBox.readSuperValue;

const staticValueSymbol = Symbol("static-value");

class SymbolBaseBox {
  static [staticValueSymbol] = 11;
}

class SymbolDerivedBox extends SymbolBaseBox {
  static readSuperSymbol() {
    return super[staticValueSymbol];
  }
}

const superSymbolReader = SymbolDerivedBox.readSuperSymbol;

class ConstructorBase {
  get value() {
    return this.tag;
  }
}

class ConstructorDerived extends ConstructorBase {
  constructor() {
    super();
    this.tag = "ctor-super";
    this.fromCtor = super.value;
  }
}

function runSuperHot() {
  let last = 0;
  for (let i = 0; i < 600; i++) {
    last = superReader();
  }
  return last;
}

function runSuperSymbolHot() {
  let last = 0;
  for (let i = 0; i < 600; i++) {
    last = superSymbolReader();
  }
  return last;
}

const isAnt = typeof process === "object"
  && process !== null
  && typeof process.versions === "object"
  && process.versions !== null
  && typeof process.versions.ant === "string";

assertEq(runNewTargetHot(), undefined, "hot inline helper preserves new.target");
assertEq(superReader(), 7, "extracted static super value lookup");
assertEq(runSuperHot(), 7, "hot direct call preserves super value lookup");
assertEq(superSymbolReader(), 11, "extracted static super symbol lookup");
assertEq(runSuperSymbolHot(), 11, "hot direct call preserves super symbol lookup");
assertEq(new ConstructorDerived().fromCtor, "ctor-super", "constructor super getter lookup");

if (isAnt) {
  const antOnly = await import("./test_jit_inline_special_obj_import.mjs");
  await antOnly.verifyImportBinding(assert, assertEq);
}

console.log("OK: test_jit_inline_special_obj");
