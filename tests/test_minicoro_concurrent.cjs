// Test that multiple async functions run concurrently
async function delay(ms, name) {
  const start = Date.now();
  await new Promise(resolve => setTimeout(resolve, ms));
  const elapsed = Date.now() - start;
  console.log(`${name}: ${elapsed}ms`);
  return elapsed;
}

console.log("Testing concurrent async/await with minicoro");
const globalStart = Date.now();

// Start both async functions - they should run concurrently
delay(100, "Task A (100ms)");
delay(50, "Task B (50ms)");

// Wait for both to complete
setTimeout(() => {
  const total = Date.now() - globalStart;
  console.log(`\nTotal time: ${total}ms`);
  
  // If concurrent: ~105ms (max of the two)
  // If sequential: ~155ms (sum of the two)
  if (total < 130) {
    console.log("✓ PASS: Tasks ran concurrently!");
  } else {
    console.log("✗ FAIL: Tasks ran sequentially");
  }
}, 120);
