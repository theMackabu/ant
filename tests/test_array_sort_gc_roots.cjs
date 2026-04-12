const assert = require("assert");

const data = {};
for (let i = 0; i < 12000; i++) {
  const key = "entry-" + String(i).padStart(5, "0") + "-" + "x".repeat(32);
  data[key] = i % 2 === 0 ? "pass" : "fail";
}

const entries = Object.entries(data).sort();

assert.equal(entries.length, 12000);
assert.equal(entries[0][0], "entry-00000-" + "x".repeat(32));
assert.equal(entries[entries.length - 1][0], "entry-11999-" + "x".repeat(32));

const nums = [];
for (let i = 2000; i >= 0; i--) nums.push(i);
nums.sort((a, b) => a - b);

assert.equal(nums[0], 0);
assert.equal(nums[nums.length - 1], 2000);

console.log("ok");
