async function test() {
  console.log("Before");
  await new Promise(resolve => setTimeout(resolve, 10));
  return "done";
}

test().then(r => console.log("Result:", r));
