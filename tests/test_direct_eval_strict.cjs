function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function strictDirectEval(source) {
  "use strict";
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

console.log("strict direct eval tests passed");
