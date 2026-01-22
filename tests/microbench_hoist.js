// Benchmark to test function declaration hoisting optimization
// The function contains "function" in strings/comments but no actual declarations

function processItem(item) {
  // This comment mentions function declaration hoisting
  // The word "function" appears here: function function function
  let type = "function";  // string contains "function"
  let desc = "This is a function test";
  let x = item * 2;
  let y = x + 1;
  return y + type.length + desc.length;
}

const iterations = 200000;
console.log(`Running ${iterations} iterations...`);

let start = Date.now();
let result = 0;

for (let i = 0; i < iterations; i++) {
  result += processItem(i);
}

console.log(`with "function" in body: ${Date.now() - start}ms (result: ${result})`);

// Compare with a function that doesn't have "function" anywhere
function processClean(item) {
  let type = "method";
  let desc = "This is a test";
  let x = item * 2;
  let y = x + 1;
  return y + type.length + desc.length;
}

start = Date.now();
result = 0;

for (let i = 0; i < iterations; i++) {
  result += processClean(i);
}

console.log(`without "function" in body: ${Date.now() - start}ms (result: ${result})`);
