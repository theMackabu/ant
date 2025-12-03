// Test that works (sync promise)
async function testSync() {
  console.log("Before await");
  const result = await Promise.resolve("SYNC");
  console.log("After await:", result);
  return "RETURNED_" + result;
}

testSync().then(r => console.log("Final:", r));
