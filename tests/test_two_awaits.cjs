// Test what happens when TWO async functions are awaiting simultaneously
console.log("=== Multiple Awaits Test ===");

async function delay(ms, name) {
  console.log(`${name}: Creating ${ms}ms delay`);
  return new Promise(resolve => {
    setTimeout(() => {
      console.log(`${name}: Timer fired after ${ms}ms`);
      resolve(`${name}_RESULT`);
    }, ms);
  });
}

async function func1() {
  console.log("func1: Before await");
  const result = await delay(100, "func1");
  console.log("func1: After await, result:", result);
  return result;
}

async function func2() {
  console.log("func2: Before await");
  const result = await delay(50, "func2");
  console.log("func2: After await, result:", result);
  return result;
}

// Call both functions - they should run concurrently
func1().then(r => console.log("func1 final:", r));
func2().then(r => console.log("func2 final:", r));

console.log("=== Both functions called ===");
