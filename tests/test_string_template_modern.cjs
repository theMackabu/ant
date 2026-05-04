function assertEq(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(label + ": expected " + JSON.stringify(expected) + ", got " + JSON.stringify(actual));
  }
}

assertEq(
  "Hello, {{name}}. Missing: {{missing}}.".template({ name: "Ant" }),
  "Hello, Ant. Missing: .",
  "missing placeholders stay empty"
);

assertEq(
  "{{nil}}/{{undef}}/{{obj}}/{{arr}}/{{ok}}".template({
    nil: null,
    undef: undefined,
    obj: { toString() { return "custom"; } },
    arr: [1, 2],
    ok: false
  }),
  "null/undefined/custom/1,2/false",
  "placeholder values use normal string coercion"
);

assertEq(
  "before {{name after".template({ name: "ignored" }),
  "before {{name after",
  "unterminated placeholders remain literal"
);

console.log("ok");
