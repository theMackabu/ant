// Code after return/throw/jmp is unreachable and is not compiled. A dead jump
// used to fix its target label's stack depth (0 after the return) before the
// live jumps were seen, so a for-of body ran three slots short and the JIT
// emitted MIR with garbage registers (MIR error: process exit).
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const source = `
  function g(m, k) { return k; }
  function deadJumpInForOf(arr, x) {
    for (const m of arr) {
      if (m.a) {
        if (m.b) return g(m, 1); else return g(m, 2);
      } else if (m.c) {
        x = x + 1;
      }
      if (x & 8) return g(m, 3);
    }
    return x;
  }
  function rethrowFromNestedCatch() {
    try {
      try { throw 'inner'; } catch (e) { throw 're: ' + e; }
    } catch (e) {
      return e;
    }
  }
  function returnInsideTry(x) {
    try {
      if (x > 0) return x;
      throw new Error('neg');
    } catch (e) {
      try { throw e.message; } catch (m) { return m + x; }
    }
  }
  const items = [{ c: 1 }, { c: 0 }, { c: 1 }];
  let s = 0, r = '';
  for (let i = 0; i < 20000; i++) {
    s += deadJumpInForOf(items, i & 7);
    r = rethrowFromNestedCatch();
    s += returnInsideTry((i & 1) ? i : -1) === 'neg-1' ? 1 : 0;
  }
  s += deadJumpInForOf([{ a: 1, b: 1 }], 0) + deadJumpInForOf([{ a: 1 }], 0) + deadJumpInForOf([{ c: 1 }, { c: 1 }], 7);
  console.log(s, r, returnInsideTry(-1), returnInsideTry(3));
`;

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-jit-unreachable-'));
try {
  const file = path.join(root, 'case.cjs');
  fs.writeFileSync(file, source);
  const jit = spawnSync(process.execPath, [file], { encoding: 'utf8' });
  const interp = spawnSync(process.execPath, ['--jitless', file], { encoding: 'utf8' });
  assert.strictEqual(jit.status, 0, jit.stderr);
  assert.strictEqual(interp.status, 0, interp.stderr);
  assert.strictEqual(jit.stdout, interp.stdout);
} finally {
  fs.rmSync(root, { recursive: true, force: true });
}

console.log('unreachable code compiles to the interpreter result');
