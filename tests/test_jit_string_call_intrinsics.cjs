function parsePath(url, pathStart) {
  const start = url.indexOf("/", pathStart);
  const query = url.indexOf("?", start);
  return url.substring(start, query === -1 ? url.length : query);
}

for (let i = 0; i < 20_000; i++) {
  if (parsePath("http://localhost/path?x=1", 16) !== "/path")
    throw new Error("warmed string call intrinsic returned the wrong path");
}

if ("a😀b".indexOf("b") !== 3) throw new Error("Unicode indexOf fallback failed");
if ("a😀b".substring(1, 3) !== "😀") throw new Error("Unicode substring fallback failed");

const originalIndexOf = String.prototype.indexOf;
const originalSubstring = String.prototype.substring;
String.prototype.indexOf = function () { return 19; };
String.prototype.substring = function () { return "patched"; };
if ("abc".indexOf("b") !== 19) throw new Error("patched indexOf was not observed");
if ("abc".substring(1) !== "patched") throw new Error("patched substring was not observed");
String.prototype.indexOf = originalIndexOf;
String.prototype.substring = originalSubstring;

const custom = {
  indexOf() { return 23; },
  substring() { return "custom"; },
};
if (custom.indexOf("x") !== 23) throw new Error("custom indexOf fallback failed");
if (custom.substring(1) !== "custom") throw new Error("custom substring fallback failed");

console.log("JIT string call intrinsic tests passed");
