// Test with IMMEDIATELY resolved promises (should work)
console.log("=== Immediate Promises Test ===");

async function func1() {
  console.log("func1: start");
  const result = await Promise.resolve("func1_result");
  console.log("func1: after await, result:", result);
  return result;
}

async function func2() {
  console.log("func2: start");
  const result = await Promise.resolve("func2_result");
  console.log("func2: after await, result:", result);
  return result;
}

func1().then(r => console.log("func1 final:", r));
func2().then(r => console.log("func2 final:", r));

console.log("=== Both called ===");
