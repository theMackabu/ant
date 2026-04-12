function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

let parserLike = "";
const parserLikeIters = 600000;
for (let i = 0; i < parserLikeIters; i++) {
  let ch = (i & 1) === 0 ? "a" : "\t";
  if (ch === "\t") ch = " ";
  parserLike += ch;
}

assert(parserLike.length === parserLikeIters, "parser-like append length mismatch");
assert(parserLike.substring(0, 6) === "a a a ", "parser-like append prefix mismatch");
assert(parserLike.substring(parserLikeIters - 6) === "a a a ", "parser-like append suffix mismatch");

let chunked = "";
for (let i = 0; i < 200000; i++) chunked += "xyz";

assert(chunked.length === 600000, "chunked append length mismatch");
assert(chunked.substring(0, 9) === "xyzxyzxyz", "chunked append prefix mismatch");
assert(chunked.substring(chunked.length - 9) === "xyzxyzxyz", "chunked append suffix mismatch");
assert(chunked.charCodeAt(3) === 120, "chunked append charCodeAt mismatch");

for (const count of [1000, 10000, 100000]) {
  let s = "";
  for (let i = 0; i < count; i++) s += "a";
  assert(s.length === count, `single-char append mismatch @ ${count}`);
  assert(s.charCodeAt(0) === 97, `charCodeAt mismatch @ ${count}`);
  assert(s.substring(count - 4) === "aaaa", `substring mismatch @ ${count}`);
}

function takesString(s) {
  return s.length;
}

let escaped = "";
for (let i = 0; i < 5000; i++) escaped = escaped + "xyz";
assert(takesString(escaped) === 15000, "call escape length mismatch");
assert(escaped.substring(0, 6) === "xyzxyz", "call escape prefix mismatch");

let assigned = "";
for (let i = 0; i < 4096; i++) assigned += (i & 1) ? "b" : "a";
const box = { value: assigned };
assert(box.value.length === 4096, "object assignment length mismatch");
assert(box.value.substring(0, 4) === "abab", "object assignment prefix mismatch");

const arr = [];
arr.push(assigned);
assert(arr[0].length === 4096, "array assignment length mismatch");
assert(arr[0].substring(0, 4) === "abab", "array assignment prefix mismatch");

let branchy = "";
for (let i = 0; i < 20000; i++) {
  if ((i & 3) === 0) branchy += "x";
  else branchy = branchy + "yz";
}
assert(branchy.length === 35000, "branchy append length mismatch");
assert(branchy.substring(0, 5) === "xyzyz", "branchy append prefix mismatch");
assert(branchy.charCodeAt(0) === 120, "branchy append charCodeAt mismatch");

let mixed = "";
for (let i = 0; i < 30000; i++) mixed += (i & 1) === 0 ? "m" : "tiny";
assert(mixed.length === 75000, "mixed append length mismatch");
assert(mixed.substring(0, 9) === "mtinymtin", "mixed append prefix mismatch");
assert(mixed.charCodeAt(1) === 116, "mixed append charCodeAt mismatch");

let numeric = 0;
for (let i = 0; i < 10000; i++) numeric += 3;
assert(numeric === 30000, "numeric += changed semantics");

const obj = { value: "" };
for (let i = 0; i < 2048; i++) obj.value += "q";
assert(obj.value.length === 2048, "non-local append length mismatch");
assert(obj.value.substring(0, 4) === "qqqq", "non-local append prefix mismatch");

console.log("small append loop test passed");
