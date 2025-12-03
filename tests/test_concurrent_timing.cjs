console.log("START");
const start = Date.now();

async function delay(ms, name) {
  console.log(`${name}: Starting ${ms}ms wait at +${Date.now() - start}ms`);
  await new Promise(resolve => setTimeout(() => {
    console.log(`${name}: Timer fired at +${Date.now() - start}ms`);
    resolve();
  }, ms));
  console.log(`${name}: Resumed at +${Date.now() - start}ms`);
  return `${name}_done`;
}

console.log("Calling func1 at +${Date.now() - start}ms");
delay(100, "func1").then(r => console.log(`func1 complete: ${r} at +${Date.now() - start}ms`));
console.log("Called func1, calling func2 at +${Date.now() - start}ms");
delay(50, "func2").then(r => console.log(`func2 complete: ${r} at +${Date.now() - start}ms`));
console.log("Called func2 at +${Date.now() - start}ms");

console.log("END at +${Date.now() - start}ms");
