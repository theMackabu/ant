const assert = require('node:assert');

let parentRuns = 0;
class A {
  constructor() { parentRuns++; this.a = 1; }
  get z() { return 9; }
}
class Replaces { constructor() { return { replaced: true }; } }

const dyn = (source) => String(source);

class Twice extends A { constructor() { super(); super(); } }
class TwiceArrow extends A { constructor() { const again = () => super(); super(); again(); } }
class TwiceBranch extends A { constructor(flag) { if (flag) super(); super(); } }
class OnceBranch extends A { constructor(flag) { if (flag) super(); else super(); } }

class NoSuper extends A { constructor() {} }
class BareReturn extends A { constructor() { return; } }
class ReturnUndefined extends A { constructor() { return undefined; } }
class EarlyReturn extends A { constructor(flag) { if (flag) return; super(); } }
class ReturnsObject extends A { constructor() { return { own: 1 }; } }
class ReturnsPrimitive extends A { constructor() { return 1; } }
class ReturnsCall extends A { constructor() { return (() => undefined)(); } }
class ReturnAfterSuper extends A { constructor() { super(); return; } }
class ArrowSuperReturn extends Replaces { constructor() { (() => super())(); return; } }

class FinallySuper extends A { constructor() { try { return; } finally { super(); } } }
class FinallyNoSuper extends A { constructor() { try { return; } finally { } } }
class FinallyObject extends A { constructor() { try { return { own: 2 }; } finally { } } }
class FinallyNested extends A { constructor() { try { try { return; } finally { } } finally { super(); } } }
class FinallyForOf extends A { constructor() { for (const x of [1]) { try { return; } finally { super(); } } } }
class FinallyCatch extends A { constructor() { try { throw 1; } catch { return; } finally { super(); } } }

class EvalThisBefore extends A { constructor() { eval(dyn('this')); super(); } }
class EvalThisAfter extends A { constructor() { super(); this.same = eval(dyn('this')) === this; } }
class EvalArrowBefore extends A { constructor() { (() => eval(dyn('this')))(); super(); } }
class EvalArrowAfter extends A { constructor() { super(); this.same = (() => eval(dyn('this')))() === this; } }
class EvalInnerArrow extends A { constructor() { eval(dyn('(() => this)()')); super(); } }
class EvalNested extends A { constructor() { eval(dyn('eval("this")')); super(); } }
class EvalSuperProp extends A { constructor() { eval(dyn('super.z')); super(); } }
class EvalSuper extends A { constructor() { eval(dyn('super()')); this.b = 1; } }
class EvalSuperTwice extends A { constructor() { eval(dyn('super()')); eval(dyn('super()')); } }
class EvalArrowSuper extends A { constructor() { (() => eval(dyn('super()')))(); this.b = 1; } }
class EvalReplaced extends Replaces { constructor() { eval(dyn('super()')); this.b = 1; } }
class EvalFields extends A { f = 5; #p = 11; read() { return this.#p; } constructor() { this.seen = undefined; } }
class EvalFieldsOk extends A {
  f = 5;
  #p = 11;
  read() { return this.#p; }
  constructor() { const seen = eval(dyn('super(); this.f')); this.seen = seen; }
}
class EvalFieldsArrow extends A { f = 8; g = () => this.f; constructor() { (() => eval(dyn('super()')))(); } }

class NestedFunctionEval extends A {
  constructor() {
    function own() { return eval(dyn('this')); }
    eval(dyn('1'));
    const seen = own.call('receiver');
    super();
    this.seen = String(seen);
  }
}

const iterations = 1500;
for (let i = 0; i < iterations; i++) {
  parentRuns = 0;
  assert.throws(() => new Twice(), ReferenceError);
  assert.strictEqual(parentRuns, 2);

  assert.throws(() => new TwiceArrow(), ReferenceError);
  assert.throws(() => new TwiceBranch(true), ReferenceError);
  assert.strictEqual(new TwiceBranch(false).a, 1);
  assert.strictEqual(new OnceBranch(i & 1).a, 1);

  assert.throws(() => new NoSuper(), ReferenceError);
  assert.throws(() => new BareReturn(), ReferenceError);
  assert.throws(() => new ReturnUndefined(), ReferenceError);
  assert.throws(() => new EarlyReturn(true), ReferenceError);
  assert.strictEqual(new EarlyReturn(false).a, 1);
  assert.deepStrictEqual(new ReturnsObject(), { own: 1 });
  assert.throws(() => new ReturnsPrimitive(), TypeError);
  assert.throws(() => new ReturnsCall(), ReferenceError);
  assert.strictEqual(new ReturnAfterSuper().a, 1);
  assert.strictEqual(new ArrowSuperReturn().replaced, true);

  assert.strictEqual(new FinallySuper().a, 1);
  assert.throws(() => new FinallyNoSuper(), ReferenceError);
  assert.deepStrictEqual(new FinallyObject(), { own: 2 });
  assert.strictEqual(new FinallyNested().a, 1);
  assert.strictEqual(new FinallyForOf().a, 1);
  assert.strictEqual(new FinallyCatch().a, 1);
}

for (let i = 0; i < 200; i++) {
  assert.throws(() => new EvalThisBefore(), ReferenceError);
  assert.strictEqual(new EvalThisAfter().same, true);
  assert.throws(() => new EvalArrowBefore(), ReferenceError);
  assert.strictEqual(new EvalArrowAfter().same, true);
  assert.throws(() => new EvalInnerArrow(), ReferenceError);
  assert.throws(() => new EvalNested(), ReferenceError);
  assert.throws(() => new EvalSuperProp(), ReferenceError);
  assert.strictEqual(new EvalSuper().b, 1);
  assert.throws(() => new EvalSuperTwice(), ReferenceError);
  assert.strictEqual(new EvalArrowSuper().b, 1);

  const replaced = new EvalReplaced();
  assert.strictEqual(replaced.replaced, true);
  assert.strictEqual(replaced.b, 1);

  assert.throws(() => new EvalFields(), ReferenceError);
  const fields = new EvalFieldsOk();
  assert.strictEqual(fields.f, 5);
  assert.strictEqual(fields.seen, 5);
  assert.strictEqual(fields.read(), 11);
  assert.strictEqual(new EvalFieldsArrow().g(), 8);

  assert.strictEqual(new NestedFunctionEval().seen, 'receiver');
}

let baseRuns = 0;
class CountedBase { constructor(v) { baseRuns++; this.v = v; } }
class SuperTwice extends CountedBase {
  constructor() { super(1); try { super(2); } catch (error) { this.error = error; } }
}
class SuperTwiceViaArrow extends CountedBase {
  constructor() { const again = (v) => super(v); again(1); try { again(2); } catch (error) { this.error = error; } }
}
for (const Class of [SuperTwice, SuperTwiceViaArrow]) {
  baseRuns = 0;
  for (let i = 0; i < 3000; i++) {
    const made = new Class();
    assert.strictEqual(made.v, 1);
    assert.ok(made.error instanceof ReferenceError);
    assert.ok(made instanceof Class);
  }
  assert.strictEqual(baseRuns, 6000);
}

function returnsPrimitive() { return 5; }
function returnsObject() { return { replaced: true }; }
const returners = { returnsPrimitive, returnsObject };
class TailPrimitive extends CountedBase { constructor() { super(); return returnsPrimitive(); } }
class TailMethodPrimitive extends CountedBase { constructor() { super(); return returners.returnsPrimitive(); } }
class TailObject extends CountedBase { constructor() { super(); return returnsObject(); } }
class TailMethodObject extends CountedBase { constructor() { super(); return returners.returnsObject(); } }
for (let i = 0; i < 3000; i++) {
  assert.throws(() => new TailPrimitive(), TypeError);
  assert.throws(() => new TailMethodPrimitive(), TypeError);
  assert.strictEqual(new TailObject().replaced, true);
  assert.strictEqual(new TailMethodObject().replaced, true);
}

console.log('ok');
