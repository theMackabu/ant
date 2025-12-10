// Test var hoisting behavior
// In JavaScript, 'var' declarations are hoisted to the function scope (or global scope)
// regardless of where they are declared within blocks

console.log("=== Test 1: Basic var hoisting in blocks ===");
{
  var x = 10;
}
console.log("x should be 10:", x);

console.log("\n=== Test 2: var in nested blocks ===");
{
  {
    {
      var y = 20;
    }
  }
}
console.log("y should be 20:", y);

console.log("\n=== Test 3: var in if blocks ===");
if (true) {
  var z = 30;
}
console.log("z should be 30:", z);

console.log("\n=== Test 4: var in function scope ===");
function testFunc() {
  {
    var a = 40;
  }
  return a;
}
console.log("a inside function should be 40:", testFunc());

console.log("\n=== Test 5: var in for loop ===");
for (var i = 0; i < 3; i++) {
  // loop body
}
console.log("i should be 3:", i);

console.log("\n=== Test 6: var in with statement ===");
var obj = { prop: 100 };
with (obj) {
  var w = 50;
}
console.log("w should be 50:", w);

console.log("\n=== Test 7: Multiple var declarations in different blocks ===");
{
  var m = 1;
}
{
  var n = 2;
}
console.log("m should be 1, n should be 2:", m, n);

console.log("\n=== Test 8: var reassignment across blocks ===");
{
  var p = 100;
}
{
  p = 200;
}
console.log("p should be 200:", p);

console.log("\n=== Test 9: var in while loop ===");
var count = 0;
while (count < 2) {
  var q = count;
  count++;
}
console.log("q should be 1:", q);

console.log("\n=== Test 10: Function scope isolation ===");
function outer() {
  {
    var funcVar = 77;
  }
  return funcVar;
}
console.log("funcVar inside function should be 77:", outer());
// funcVar should not be accessible here (would be undefined in global scope)

console.log("\n=== Test 11: var vs let comparison ===");
{
  var varTest = "var-value";
  let letTest = "let-value";
}
console.log("varTest should be 'var-value':", varTest);
try {
  console.log("letTest should cause error:", letTest);
} catch (e) {
  console.log("letTest correctly not accessible (block-scoped)");
}

console.log("\n=== All var hoisting tests completed ===");
