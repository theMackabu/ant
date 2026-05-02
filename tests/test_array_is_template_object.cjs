const assert = (cond, msg) => {
  if (!cond) throw new Error(msg);
};

let captured;
function tag(strings) {
  captured = strings;
  return strings;
}

const first = tag`a${1}b`;
const sameTextDifferentSite = tag`a${2}b`;

function sameSite(value) {
  return tag`a${value}b`;
}

const sameSiteFirst = sameSite(1);
const sameSiteSecond = sameSite(2);

assert(typeof Array.isTemplateObject === "function", "Array.isTemplateObject should exist");
assert(Array.isTemplateObject(first) === true, "tagged template strings array should be a template object");
assert(Array.isTemplateObject(first.raw) === false, "raw array should not be a template object");
assert(Array.isTemplateObject([]) === false, "ordinary array should not be a template object");
assert(Array.isTemplateObject(Object.freeze(["a", "b"])) === false, "frozen ordinary array should not be a template object");
assert(Array.isTemplateObject({ raw: ["a"] }) === false, "ordinary object should not be a template object");
assert(Array.isTemplateObject("not an array") === false, "primitive should not be a template object");
assert(Array.isTemplateObject() === false, "missing argument should return false");
assert(first !== sameTextDifferentSite, "different template sites should not reuse the same object");
assert(sameSiteFirst === sameSiteSecond, "same template site should reuse the same object");
assert(captured === sameSiteSecond, "tag should receive cached template object");

console.log("Array.isTemplateObject tests ok");
