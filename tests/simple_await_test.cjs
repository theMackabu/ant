async function test() {
  await new Promise(resolve => setTimeout(resolve, 10));
  return "done";
}
test().then(r => console.log(r));
