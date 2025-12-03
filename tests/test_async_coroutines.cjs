// Test async/await with coroutines
console.log("Starting async test");

async function delay(ms) {
  return new Promise((resolve) => {
    setTimeout(() => {
      console.log(`Delayed ${ms}ms`);
      resolve(`Result after ${ms}ms`);
    }, ms);
  });
}

async function main() {
  console.log("Before await 1");
  const result1 = await delay(100);
  console.log("After await 1:", result1);
  
  console.log("Before await 2");
  const result2 = await delay(50);
  console.log("After await 2:", result2);
  
  return "All done!";
}

// Call the async function
main().then(result => {
  console.log("Final result:", result);
}).catch(err => {
  console.error("Error:", err);
});

console.log("After main() call (should execute before awaits)");
