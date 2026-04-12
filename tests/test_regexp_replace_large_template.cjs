const assert = require("assert");

const chunk = "abc-123-def-456-ghi-789";
const input = Array(2048).fill(chunk).join("|");
const re = /([a-z]+)-(\d+)-([a-z]+)-(\d+)-([a-z]+)-(\d+)/g;
const replacement = "$1:$2:$3:$4:$5:$6::" + "X".repeat(2048) + "::$&::$`::$'";

const out = input.replace(re, replacement);

assert.equal(typeof out, "string");
assert.ok(out.includes("abc:123:def:456:ghi:789"));
assert.ok(out.includes("X".repeat(256)));
assert.ok(out.length > input.length);

console.log("ok");
