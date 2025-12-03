// Test arrow function closures
console.log("Test 1: Basic arrow closure");
function outer(x) {
  return () => x;
}
const fn = outer(42);
console.log("Result:", fn());

console.log("\nTest 2: Arrow in setTimeout");
function test2(value) {
  setTimeout(() => {
    console.log("Value in setTimeout:", value);
  }, 10);
}
test2(123);

console.log("\nTest 3: Promise with arrow closure");
function test3(num) {
  return new Promise((resolve) => {
    setTimeout(() => {
      console.log("Num in promise:", num);
      resolve(num * 2);
    }, 10);
  });
}

test3(5).then(result => {
  console.log("Promise resolved with:", result);
});

console.log("Synchronous code done");
