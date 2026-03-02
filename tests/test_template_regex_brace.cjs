function assertEq(actual, expected, label) {
  if (actual !== expected) {
    throw new Error(`${label}: expected '${expected}' got '${actual}'`);
  }
}

const _ = "";
const code = "foo";
const generated = `export default${_ || (/^[{[\-\/]/.test(code) ? "" : " ")}${code};`;
assertEq(generated, "export default foo;", "generated template");

const rootPath = "resolve";
const key = "alias";
const dotted = `${rootPath}.${key}`;
assertEq(dotted, "resolve.alias", "dotted template");

console.log("ok");
