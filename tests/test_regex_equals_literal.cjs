const EQUAL_RE = /=/g;
const x = "a=b=c";
const out = x.replace(EQUAL_RE, ":");

if (out !== "a:b:c") {
  throw new Error("regex '/=/' literal parsed incorrectly");
}

console.log("ok");
