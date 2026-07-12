function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function strictDirectEval(source) {
  "use strict";
  return eval(source);
}

function sloppyDirectEval(source) {
  return eval(source);
}

function assertStrictEvalSyntaxError(source, label) {
  try {
    strictDirectEval(source);
  } catch (error) {
    assert(error instanceof SyntaxError, label + " should throw SyntaxError");
    return;
  }
  throw new Error(label + " should fail during strict direct eval");
}

assertStrictEvalSyntaxError("with ({}) {}", "with statement");
assertStrictEvalSyntaxError("delete binding", "deleting a binding");
assertStrictEvalSyntaxError("function duplicate(value, value) {}", "duplicate parameters");
assertStrictEvalSyntaxError("var implements", "strict reserved binding");
assertStrictEvalSyntaxError("var eval", "eval binding");
assertStrictEvalSyntaxError("var arguments", "arguments binding");
assertStrictEvalSyntaxError("eval = 1", "eval assignment");
assertStrictEvalSyntaxError("arguments = 1", "arguments assignment");
assertStrictEvalSyntaxError("010", "legacy octal literal");

function assertStrictEvalUsesRealmSyntaxError(source) {
  "use strict";
  const SyntaxError = function LocalSyntaxError() {};
  try {
    eval(source);
  } catch (error) {
    assert(
      error instanceof globalThis.SyntaxError,
      "strict direct eval errors should use the realm SyntaxError prototype"
    );
    assert(
      !(error instanceof SyntaxError),
      "a lexical SyntaxError binding must not replace the realm intrinsic"
    );
    return;
  }
  throw new Error("shadowed SyntaxError case should fail during strict direct eval");
}

assertStrictEvalUsesRealmSyntaxError("with ({}) {}");

assert(
  sloppyDirectEval("globalThis") === globalThis,
  "direct eval globalThis should be the realm global object"
);
assert(
  sloppyDirectEval("(function () { return this; })()") === globalThis,
  "a sloppy function called inside direct eval should receive the realm global this"
);

const evalFunction = sloppyDirectEval("(function evalFunction() {})");
assert(
  Object.getPrototypeOf(evalFunction) === Function.prototype,
  "functions created by direct eval should use the realm Function prototype"
);

const evalGenerator = sloppyDirectEval("(function* evalGenerator() {})");
const realmGenerator = function* realmGenerator() {};
assert(
  Object.getPrototypeOf(evalGenerator) === Object.getPrototypeOf(realmGenerator),
  "generators created by direct eval should use the realm generator prototype"
);

const evalClass = sloppyDirectEval("(class EvalClass {})");
assert(
  Object.getPrototypeOf(evalClass) === Function.prototype,
  "classes created by direct eval should use the realm Function prototype"
);
assert(
  Object.getPrototypeOf(evalClass.prototype) === Object.prototype,
  "class prototypes created by direct eval should use the realm Object prototype"
);

console.log("strict direct eval tests passed");
