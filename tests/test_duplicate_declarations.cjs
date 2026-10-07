// A name may be declared once per scope lexically (let, const, class, using,
// import, functions in blocks and module top level); it may not also be a
// var, a parameter, or a catch parameter of that scope. Sloppy-mode duplicate
// function declarations in a block, var redeclarations and shadowing in inner
// scopes stay legal. The answers match Node.
const assert = require('node:assert');
const { spawnSync } = require('node:child_process');

const cases = [
  [
    "const a = 1; const a = 2;",
    true
  ],
  [
    "let b = 1; let b = 2;",
    true
  ],
  [
    "const c = 1; var c = 2;",
    true
  ],
  [
    "var c2 = 1; let c2 = 2;",
    true
  ],
  [
    "function f() {} let f = 1;",
    true
  ],
  [
    "let f2 = 1; function f2() {}",
    true
  ],
  [
    "class K {} let K = 1;",
    true
  ],
  [
    "{ let d; let d; }",
    true
  ],
  [
    "{ let d2; var d2; }",
    true
  ],
  [
    "{ let d3; { var d3; } }",
    true
  ],
  [
    "{ var e; let e; }",
    true
  ],
  [
    "let g; { var g; }",
    true
  ],
  [
    "{ let h; } var h;",
    false
  ],
  [
    "{ function i() {} function i() {} }",
    false
  ],
  [
    "'use strict'; { function j() {} function j() {} }",
    true
  ],
  [
    "{ function k() {} let k; }",
    true
  ],
  [
    "{ let k2; function k2() {} }",
    true
  ],
  [
    "function m(a) { let a; }",
    true
  ],
  [
    "function m2(a) { var a; return a; } m2(1);",
    false
  ],
  [
    "function m3(a) { function a() {} }",
    false
  ],
  [
    "function m4([a, b]) { const b = 1; }",
    true
  ],
  [
    "(a) => { let a; };",
    true
  ],
  [
    "(a) => { var a; };",
    false
  ],
  [
    "for (let n = 0; ;) { var n; break; }",
    true
  ],
  [
    "for (let n2 of []) { var n2; }",
    true
  ],
  [
    "for (let n3 = 0, n3 = 1; ;) break;",
    true
  ],
  [
    "for (const n4 in {}) { let n4; }",
    false
  ],
  [
    "for (var n5 = 0; n5 < 1; n5++) { let n5; }",
    false
  ],
  [
    "try {} catch (e) { let e; }",
    true
  ],
  [
    "try {} catch (e) { var e; }",
    false
  ],
  [
    "try {} catch ([e]) { var e; }",
    true
  ],
  [
    "try {} catch ({ e }) { let x; }",
    false
  ],
  [
    "try {} catch (e) { { let e; } }",
    false
  ],
  [
    "switch (1) { case 1: let p; case 2: let p; }",
    true
  ],
  [
    "switch (1) { case 1: let p2; break; default: var p2; }",
    true
  ],
  [
    "switch (1) { case 1: function q() {} case 2: function q() {} }",
    false
  ],
  [
    "var r; var r; function r() {}",
    false
  ],
  [
    "function s() {} function s() {}",
    false
  ],
  [
    "let t; { let t; { let t; } }",
    false
  ],
  [
    "function u() { let arguments; return 1; } u();",
    false
  ],
  [
    "(function v() { let v = 2; return v; })();",
    false
  ],
  [
    "label: { let w; let w; }",
    true
  ],
  [
    "if (true) function x1() {} let x1;",
    false
  ],
  [
    "class L { m() { let L; } static { let z; var z2; } }",
    false
  ],
  [
    "class M { static { let z; let z; } }",
    true
  ],
  [
    "const { y1, y2: y1 } = {};",
    true
  ],
  [
    "let [aa, aa] = [];",
    true
  ],
  [
    "var bb; var [bb] = [];",
    false
  ],
  [
    "eval('let dd; var dd;');",
    true
  ],
  [
    "eval('let ee; let ff;'); console.log('evalok');",
    false
  ]
];

const env = { ...process.env, NO_COLOR: '1' };
delete env.FORCE_COLOR;
for (const [code, rejected] of cases) {
  const child = spawnSync(process.execPath, ['-e', code], { encoding: 'utf8', timeout: 30000, env });
  const got = /SyntaxError/.test(child.stderr) && /has already been declared/.test(child.stderr);
  assert.strictEqual(got, rejected, `${rejected ? 'should reject' : 'should accept'}: ${code}\n${child.stderr}`);
}
console.log(`PASS ${cases.length} declaration conflict cases match Node`);
