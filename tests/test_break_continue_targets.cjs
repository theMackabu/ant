const assert = require('node:assert');

const rejected = [
  "break;",
  "continue;",
  "function f(){ break; }",
  "function f(){ continue; }",
  "x: { continue x; }",
  "break nolabel;",
  "for(;;){ break nolabel; }",
  "for(;;){ continue nolabel; }",
  "a: for(;;){ function g(){ break a; } }",
  "a: for(;;){ (() => { break a; }) }",
  "switch(1){ case 1: continue; }",
  "x: x: ;",
  "if (1) break;",
  "a: { for(;;) { continue a; } }",
  "class A { static { break; } }",
  "for(;;){ class A { static { break; } } }",
  "for(;;){ class A { static { continue; } } }",
  "l: for(;;){ class A { static { break l; } } }",
  "l: for(;;){ class A { static { continue l; } } }",
  "x: { break; }",
  "a: { b: for(;;){ continue a; } }",
  "a: switch(1){ case 1: continue a; }",
  "a: for(;;){ a: for(;;){} }",
  "try { break; } finally {}",
  "with({}) { break; }",
];
for (const source of rejected) assert.throws(() => new Function(source), SyntaxError, source);

const accepted = [
  "for(;;){ switch(1){ case 1: continue; } break; }",
  "x: { break x; }",
  "a: b: for(;;){ continue a; }",
  "a: b: for(;;){ break a; }",
  "a: switch(1){ case 1: break a; }",
  "for(;;){ x: { continue; } }",
  "a: for(;;){} a: for(;;){}",
  "a: { (function(){ a: ; }) }",
  "do break; while(0)",
  "switch(1){ default: break; }",
  "for(;;){ try { continue; } finally {} }",
  "label: function f(){}",
];
for (const source of accepted) assert.doesNotThrow(() => new Function(source), source);

const indirectEval = eval;
assert.throws(() => indirectEval('for (;;) { class A { static { break; } } }'), SyntaxError);
assert.throws(() => { for (let i = 0; i < 1; i++) eval('break'); }, SyntaxError);

function labels() {
  let s = '';
  for (let i = 0; i < 3; i++) { x: { if (i === 1) continue; s += i; } }
  s += '|';
  a: b: for (let i = 0; i < 3; i++) { for (;;) { s += i; continue a; } }
  s += '|';
  a: b: for (let i = 0; i < 3; i++) { for (;;) { s += i; break a; } }
  s += '|';
  a: { b: for (let i = 0; i < 3; i++) { s += i; break a; } }
  s += '|';
  a: b: c: for (let i = 0; i < 4; i++) {
    if (i === 0) continue a;
    if (i === 1) continue b;
    if (i === 2) continue c;
    s += i;
    break b;
  }
  s += '|';
  sw: switch (1) { case 1: for (let i = 0; i < 3; i++) { if (i === 1) break sw; s += i; } s += 'unreached'; }
  s += '|';
  for (let i = 0; i < 3; i++) { switch (i) { case 1: continue; default: s += i; } }
  s += '|';
  for (let i = 0; i < 3; i++) { try { if (i === 1) continue; s += i; } finally { s += 'f'; } }
  s += '|';
  a: do { s += 'd'; continue a; } while (false);
  return s;
}
for (let round = 0; round < 3; round++) assert.strictEqual(labels(), '02|012|0|0|3|0|02|0ff2f|d');

console.log('ok');
