const pkg = require("cond-exports");

if (pkg.branch !== "require") {
  console.log("FAIL: require() should prefer package exports.require, got", pkg.branch);
} else {
  console.log("PASS");
}
