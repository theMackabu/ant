const assert = require("node:assert");
const { spawnSync } = require("child_process");

const source = [
  "async function explode(){ throw new Error('boom') }",
  "explode().then(() => {}).catch(() => console.log('chained-caught'))",
].join(" ");

const result = spawnSync(process.execPath, ["-e", source], {
  encoding: "utf8",
});

if (result.error) {
  throw result.error;
}

assert.strictEqual(result.status, 0, result.stderr || result.stdout);
assert.match(result.stdout, /chained-caught/);
assert.doesNotMatch(result.stderr, /Uncaught \\(in promise\\)/);

console.log("promise chains with downstream catch are not reported as unhandled");
