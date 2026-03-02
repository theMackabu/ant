let failed = 0;

function SourceLike(aLine, aColumn, aSource, aChunks, aName) {
  this.children = [];
  this.line = aLine == null ? null : aLine;
  this.column = aColumn == null ? null : aColumn;
  this.source = aSource == null ? null : aSource;
  this.name = aName == null ? null : aName;
  if (aChunks != null) this.add(aChunks);
}

SourceLike.prototype.add = function add(aChunk) {
  if (Array.isArray(aChunk)) {
    aChunk.forEach(function (chunk) {
      this.add(chunk);
    }, this);
    return;
  }
  if (typeof aChunk === "string") {
    this.children.push(aChunk);
    return;
  }
  throw new Error("unexpected chunk type: " + typeof aChunk);
};

const driver = {
  merge: function merge() {
    for (let i = 0; i < 3000; i++) {
      const s = new SourceLike(1, 2, 3);
      s.add(["x"]);
    }
  },
};

try {
  driver.merge();
} catch (e) {
  failed++;
  console.log("fail:", e && e.message ? e.message : e);
}

if (failed > 0) throw new Error("test_jit_missing_arg_normalization failed");
