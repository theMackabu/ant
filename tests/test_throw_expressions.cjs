const assert = require("assert");

{
  const fn = () => throw 42;
  try {
    fn();
    assert(false, "arrow throw expression should throw");
  } catch (error) {
    assert.strictEqual(error, 42);
  }
}

{
  true ? 1 : throw 2;
  try {
    false ? 1 : throw 2;
    assert(false, "conditional throw expression should throw");
  } catch (error) {
    assert.strictEqual(error, 2);
  }
}

{
  let a;
  try {
    a = 19 || throw 77;
    88 && throw 23;
    assert(false, "logical throw expression should throw");
  } catch (error) {
    assert.strictEqual(a + error, 42);
  }
}

{
  function fn(arg = throw 42) {
    return arg;
  }

  assert.strictEqual(fn(21), 21);
  try {
    fn();
    assert(false, "parameter initializer throw expression should throw");
  } catch (error) {
    assert.strictEqual(error, 42);
  }
}

console.log("throw expression tests completed!");
