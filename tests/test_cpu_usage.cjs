async function test() {
  console.log("Starting wait...");
  await new Promise(resolve => setTimeout(resolve, 500));
  console.log("Done!");
  return "success";
}

test().then(r => console.log("Result:", r));
