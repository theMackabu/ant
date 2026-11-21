const assert = require('node:assert');

const N = 8000;
const names = (stack, stop) => {
  const out = [];
  for (const line of String(stack).split('\n').slice(1)) {
    const match = /^\s*at (?:async )?(?:new )?([^ (]+)/.exec(line);
    if (!match) continue;
    const name = match[1].replace(/^.*\./, '');
    out.push(name);
    if (name === stop) break;
  }
  return out;
};
const countFrames = (stack) => String(stack).split('\n').slice(1).filter((line) => /^\s*at /.test(line)).length;

class Base { constructor(i) { this.x = i; if (i === N) this.stack = new Error('base').stack; } }
class Middle extends Base { constructor(i) { super(i); this.m = i; if (i === N + 1) this.stack = new Error('middle').stack; } }
class Leaf extends Middle { constructor(i) { super(i); this.l = 1; } }
function build(i) { const made = new Leaf(i); return made; }

function ctorChain(n) {
  const seen = [];
  for (let i = 0; i < n; i++) {
    try {
      const made = build(i);
      if (made.stack) seen.push(names(made.stack, 'ctorChain'));
    } catch (error) { seen.push(error); }
  }
  return [...seen];
}
ctorChain(300);
assert.deepStrictEqual(ctorChain(N + 2), [
  ['Base', 'Middle', 'Leaf', 'build', 'ctorChain'],
  ['Middle', 'Leaf', 'build', 'ctorChain'],
]);

class Thrower { constructor(i) { if (i === N) throw new Error('thrower'); this.x = i; } }
class ThrowerChild extends Thrower { constructor(i) { super(i); this.c = 1; } }
function buildThrower(i) { const made = new ThrowerChild(i); return made; }
function thrownChain(n) {
  const seen = [];
  for (let i = 0; i < n; i++) {
    try { buildThrower(i); } catch (error) { seen.push(names(error.stack, 'thrownChain')); }
  }
  return [...seen];
}
thrownChain(300);
assert.deepStrictEqual(thrownChain(N + 1), [['Thrower', 'ThrowerChild', 'buildThrower', 'thrownChain']]);

class Animal { speak(i) { if (i === N) return new Error('speak').stack; return i; } }
class Dog extends Animal { speak(i) { const said = super.speak(i); return said; } }
class Puppy extends Dog { speak(i) { const said = super.speak(i); if (i === N + 1) return new Error('puppy').stack; return said; } }
const puppy = new Puppy();
function speak(i) { const said = puppy.speak(i); return said; }
function superMethods(n) {
  const seen = [];
  for (let i = 0; i < n; i++) {
    try {
      const said = speak(i);
      if (typeof said === 'string') seen.push(names(said, 'superMethods'));
    } catch (error) { seen.push(error); }
  }
  return [...seen];
}
superMethods(300);
assert.deepStrictEqual(superMethods(N + 2), [
  ['speak', 'speak', 'speak', 'speak', 'superMethods'],
  ['speak', 'speak', 'superMethods'],
]);

class Checked { check(i) { if (i === N) throw new Error('checked'); return i; } }
class CheckedChild extends Checked { check(i) { const value = super.check(i); return value + 1; } }
const checked = new CheckedChild();
function check(i) { const value = checked.check(i); return value; }
function thrownMethods(n) {
  const seen = [];
  for (let i = 0; i < n; i++) {
    try { check(i); } catch (error) { seen.push(names(error.stack, 'thrownMethods')); }
  }
  return [...seen];
}
thrownMethods(300);
assert.deepStrictEqual(thrownMethods(N + 1), [['check', 'check', 'check', 'thrownMethods']]);

class Shaped { constructor(v) { this.x = v.y; if (v.grab) this.stack = new Error('shaped').stack; } }
class ShapedChild extends Shaped { constructor(v) { super(v); this.c = v.y; } }
function buildShaped(v) { const made = new ShapedChild(v); return made; }
function shapeChain(n, plain, odd) {
  const seen = [];
  for (let i = 0; i < n; i++) {
    try {
      const made = buildShaped(i === N ? odd : plain);
      if (made.stack) seen.push(names(made.stack, 'shapeChain'));
    } catch (error) { seen.push(error); }
  }
  return [...seen];
}
shapeChain(300, { y: 1 }, { y: 1 });
assert.deepStrictEqual(shapeChain(N + 1, { y: 1 }, { grab: true, extra: 1, y: 2 }), [['Shaped', 'ShapedChild', 'buildShaped', 'shapeChain']]);

class Top { constructor(i) { this.x = i; if (i === N) this.stack = new Error('top').stack; else if (i === -1) this.stack = new Error('cold').stack; } }
class TopMiddle extends Top { constructor(i) { super(i); this.m = i; if (i === N + 1) this.stack = new Error('middle').stack; else if (i === -2) this.stack = new Error('cold').stack; } }
class TopLeaf extends TopMiddle { constructor(i) { super(i); this.l = 1; } }
function buildTop(i) { const made = new TopLeaf(i); return made; }
class Voice { say(i) { if (i === N) return new Error('say').stack; if (i === -1) return new Error('cold').stack; return i; } }
class Louder extends Voice { say(i) { const said = super.say(i); if (i === N + 1) return new Error('louder').stack; if (i === -2) return new Error('cold').stack; return said; } }
const louder = new Louder();
function say(i) { const said = louder.say(i); return said; }
class Fails { constructor(i) { if (i === N) throw new Error('fails'); if (i === -1) throw new Error('cold'); this.x = i; } }
class FailsChild extends Fails { constructor(i) { super(i); this.c = 1; } }
function buildFails(i) { const made = new FailsChild(i); return made; }
let coldFails = 0;
try { buildFails(-1); } catch (error) { coldFails = countFrames(error.stack); }

const cold = [countFrames(buildTop(-1).stack), countFrames(buildTop(-2).stack), countFrames(say(-1)), countFrames(say(-2)), coldFails];
const topLevel = [];
for (let i = 0; i < N + 2; i++) {
  try {
    const made = buildTop(i);
    if (i >= N) topLevel.push(names(made.stack, 'buildTop'), countFrames(made.stack) - cold[i - N]);
  } catch (error) { topLevel.push(error); }
}
for (let i = 0; i < N + 2; i++) {
  try {
    const said = say(i);
    if (i >= N) topLevel.push(names(said).slice(0, 3 - (i - N)), countFrames(said) - cold[2 + i - N]);
  } catch (error) { topLevel.push(error); }
}
for (let i = 0; i < N + 2; i++) {
  try { buildFails(i); } catch (error) { topLevel.push(names(error.stack, 'buildFails'), countFrames(error.stack) - cold[4]); }
}
assert.deepStrictEqual([...topLevel], [
  ['Top', 'TopMiddle', 'TopLeaf', 'buildTop'], 0, ['TopMiddle', 'TopLeaf', 'buildTop'], 0,
  ['say', 'say', 'say'], 0, ['say', 'say'], 0,
  ['Fails', 'FailsChild', 'buildFails'], 0,
]);

console.log('ok');
