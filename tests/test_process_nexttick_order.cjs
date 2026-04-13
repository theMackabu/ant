const out = [];

Promise.resolve().then(() => out.push("promise"));
process.nextTick(() => out.push("tick"));

setTimeout(() => {
  const got = JSON.stringify(out);
  const want = JSON.stringify(["tick", "promise"]);
  if (got !== want) throw new Error(`expected ${want}, got ${got}`);
  console.log("process.nextTick ordering ok");
}, 0);
