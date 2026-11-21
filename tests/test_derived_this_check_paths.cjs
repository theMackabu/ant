const assert = require('node:assert');

class A { constructor() { this.x = 1; } }
const touch = (v) => v;

class StraightLine extends A { constructor() { touch(this); touch(this); super(); } }
class IfOnly extends A { constructor(c) { if (c) super(); this.y = this.x; } }
class IfEmptyElse extends A { constructor(c) { if (c) super(); else { touch(c); } this.y = this.x; } }
class IfBoth extends A { constructor(c) { if (c) super(); else super(); this.y = this.x + this.x; } }
class IfThenReads extends A { constructor(c) { super(); if (c) { this.y = this.x; } else { this.y = 2; } this.z = this.y; } }
class AndSuper extends A { constructor(c) { c && super(); this.y = this.x; } }
class TernarySuper extends A { constructor(c) { c ? super() : 0; this.y = this.x; } }
class CatchReads extends A { constructor(c) { try { if (c) throw 1; super(); this.y = this.x; } catch (e) { this.y = e; } } }
class FinallyReads extends A { constructor(c) { try { if (c) return; super(); } finally { this.y = 3; } } }
class LoopReads extends A { constructor(xs) { for (const i of xs) { if (i) super(); this.y = this.x; } } }
class SwitchFalls extends A { constructor(c) { switch (c) { case 1: super(); case 2: this.y = this.x; } } }
class LabelSkips extends A { constructor(c) { skip: { if (c) break skip; super(); touch(this); } this.y = this.x; } }
class DoWhile extends A { constructor(c) { let n = 0; do { if (n++ > 0 || c) { super(); break; } else touch(this); } while (n < 2); } }
class ArrowReads extends A { constructor(c) { const read = () => { touch(this); return this; }; const call = () => super(); if (c) call(); this.r = read(); } }
class ArrowBranch extends A { constructor(c) { const f = () => { c && super(); touch(this); return this.x; }; const y = f(); this.y = y; } }
class NestedIf extends A { constructor(a, b) { if (a) { if (b) super(); } else super(); this.y = this.x; } }
class WhileSuper extends A { constructor(n) { while (n-- > 0) { super(); touch(this); } this.y = this.x; } }
class OptionalRead extends A { constructor(c) { c?.go && super(); this.y = this.x; } }

for (let i = 0; i < 300; i++) {
  assert.throws(() => new StraightLine(), ReferenceError);
  assert.strictEqual(new IfOnly(true).y, 1);
  assert.throws(() => new IfOnly(false), ReferenceError);
  assert.strictEqual(new IfEmptyElse(true).y, 1);
  assert.throws(() => new IfEmptyElse(false), ReferenceError);
  assert.strictEqual(new IfBoth(true).y, 2);
  assert.strictEqual(new IfBoth(false).y, 2);
  assert.strictEqual(new IfThenReads(true).z, 1);
  assert.strictEqual(new IfThenReads(false).z, 2);
  assert.strictEqual(new AndSuper(true).y, 1);
  assert.throws(() => new AndSuper(false), ReferenceError);
  assert.strictEqual(new TernarySuper(true).y, 1);
  assert.throws(() => new TernarySuper(false), ReferenceError);
  assert.strictEqual(new CatchReads(false).y, 1);
  assert.throws(() => new CatchReads(true), ReferenceError);
  assert.strictEqual(new FinallyReads(false).y, 3);
  assert.throws(() => new FinallyReads(true), ReferenceError);
  assert.strictEqual(new LoopReads([1]).y, 1);
  assert.throws(() => new LoopReads([0, 1]), ReferenceError);
  assert.strictEqual(new SwitchFalls(1).y, 1);
  assert.throws(() => new SwitchFalls(2), ReferenceError);
  assert.strictEqual(new LabelSkips(false).y, 1);
  assert.throws(() => new LabelSkips(true), ReferenceError);
  assert.strictEqual(new DoWhile(true).x, 1);
  assert.throws(() => new DoWhile(false), ReferenceError);
  assert.strictEqual(new ArrowReads(true).r.x, 1);
  assert.throws(() => new ArrowReads(false), ReferenceError);
  assert.strictEqual(new ArrowBranch(true).y, 1);
  assert.throws(() => new ArrowBranch(false), ReferenceError);
  assert.strictEqual(new NestedIf(true, true).y, 1);
  assert.strictEqual(new NestedIf(false, false).y, 1);
  assert.throws(() => new NestedIf(true, false), ReferenceError);
  assert.strictEqual(new WhileSuper(1).y, 1);
  assert.throws(() => new WhileSuper(0), ReferenceError);
  assert.throws(() => new WhileSuper(2), ReferenceError);
  assert.strictEqual(new OptionalRead({ go: 1 }).y, 1);
  assert.throws(() => new OptionalRead(null), ReferenceError);
}

console.log('ok');
