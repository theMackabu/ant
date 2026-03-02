function pickFiles(basedir, relfiles) {
  if (relfiles) var files = relfiles.map((r) => `${basedir}/${r}`);
  else var files = basedir;
  return files;
}

const a = pickFiles("/tmp", ["a.js", "b.js"]);
if (!Array.isArray(a) || a.length !== 2 || a[0] !== "/tmp/a.js") {
  throw new Error("if-branch var statement failed");
}

const b = pickFiles("/tmp/base", null);
if (b !== "/tmp/base") {
  throw new Error("else-branch var statement failed");
}

console.log("ok");
