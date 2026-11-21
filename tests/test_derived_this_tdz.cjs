const assert = require('node:assert');

let setterCalls = 0;
class Base {
  constructor(tag) { this.tag = tag; }
  set x(v) { setterCalls++; }
  get y() { return 'base-y'; }
}

class ReadsThis extends Base { constructor() { this.a = 1; super(); } }
class ReadsSuperProp extends Base { constructor() { super.y; super(); } }
class WritesSuperProp extends Base { constructor() { super.x = 1; super(); } }
class DeletesSuperProp extends Base { constructor() { delete super.x; super(); } }

let keyEvaluated = false;
class ComputedSuperKey extends Base {
  constructor() {
    try { super[(keyEvaluated = true, 'y')]; } catch (e) { super(); this.error = e; return; }
    super();
  }
}

class ArrowBeforeSuper extends Base {
  constructor() {
    const read = () => this;
    try { read(); } catch (e) { super('arrow'); this.error = e; this.after = read(); }
  }
}

class ArrowCallsSuper extends Base {
  constructor() {
    const init = () => super('from-arrow');
    init();
    this.own = this.tag;
  }
}

class ConditionalSuper extends Base {
  constructor(flag) {
    if (flag) super('yes');
    else super('no');
    this.flag = flag;
  }
}

class Replaced { constructor() { return { replaced: true }; } }
class SeesReplacement extends Replaced {
  constructor() {
    const seen = () => this;
    super();
    this.same = seen() === this;
  }
}

class FieldArrow extends Base {
  read = () => this.tag;
  constructor() {
    const early = () => this;
    super('field');
    this.early = early() === this;
  }
}

class PlainDerived extends Base {
  constructor(v) { super(v); this.v = v * 2; }
}

for (let i = 0; i < 3000; i++) {
  assert.throws(() => new ReadsThis(), ReferenceError);
  assert.throws(() => new ReadsSuperProp(), ReferenceError);

  setterCalls = 0;
  assert.throws(() => new WritesSuperProp(), ReferenceError);
  assert.strictEqual(setterCalls, 0);

  assert.throws(() => new DeletesSuperProp(), ReferenceError);

  keyEvaluated = false;
  const computed = new ComputedSuperKey();
  assert.ok(computed.error instanceof ReferenceError);
  assert.strictEqual(keyEvaluated, false);

  const arrow = new ArrowBeforeSuper();
  assert.ok(arrow.error instanceof ReferenceError);
  assert.strictEqual(arrow.after, arrow);
  assert.strictEqual(arrow.tag, 'arrow');

  const viaArrow = new ArrowCallsSuper();
  assert.strictEqual(viaArrow.own, 'from-arrow');

  assert.strictEqual(new ConditionalSuper(i & 1).tag, i & 1 ? 'yes' : 'no');

  const replaced = new SeesReplacement();
  assert.strictEqual(replaced.replaced, true);
  assert.strictEqual(replaced.same, true);

  const field = new FieldArrow();
  assert.strictEqual(field.read(), 'field');
  assert.strictEqual(field.early, true);

  const plain = new PlainDerived(i);
  assert.strictEqual(plain.tag, i);
  assert.strictEqual(plain.v, i * 2);
}

class Invoker { constructor(cb) { this.result = cb(); } }
class CallbackReadsThis extends Invoker { constructor() { super(() => this); } }
class CallbackIgnoresThis extends Invoker { constructor() { const double = (x) => x * 2; super(() => double(21)); } }
class BranchOnly extends Base { constructor(flag) { if (flag) super('a'); else super('b'); } }

for (let i = 0; i < 3000; i++) {
  assert.throws(() => new CallbackReadsThis(), ReferenceError);
  assert.strictEqual(new CallbackIgnoresThis().result, 42);
  assert.strictEqual(new BranchOnly(i & 1).tag, i & 1 ? 'a' : 'b');
}

class DeleteAfterSuper extends Base {
  constructor() { super(); delete super.x; }
}
assert.throws(() => new DeleteAfterSuper(), ReferenceError);

class Method extends Base { m() { return delete super.y; } }
assert.throws(() => new Method().m(), ReferenceError);

class ParamReadsThis extends Base { constructor(a = this) { super(); } }
class ParamArrowReadsThis extends Base { constructor(read = () => this) { read(); super(); } }
class ParamArrowAfterSuper extends Base { constructor(read = () => this) { super(); this.same = read() === this; } }
class ParamPatternReadsThis extends Base { constructor({ a = this } = {}) { super(); } }
class ParamReadsSuperProperty extends Base { constructor(a = super.y) { super(); } }
class ParamCallsSuper extends Base { constructor(self = super('from-param')) { this.same = self === this; } }
class ParamArrowCallsSuper extends Base { constructor(init = () => super('from-arrow')) { init(); this.done = true; } }
class ParamAndBodyCallSuper extends Base { constructor(self = super()) { super(); } }
class ParamSkipped extends Base { constructor(a = this) { super(a); } }

for (let i = 0; i < 3000; i++) {
  assert.throws(() => new ParamReadsThis(), ReferenceError);
  assert.throws(() => new ParamArrowReadsThis(), ReferenceError);
  assert.strictEqual(new ParamArrowAfterSuper().same, true);
  assert.throws(() => new ParamPatternReadsThis(), ReferenceError);
  assert.throws(() => new ParamReadsSuperProperty(), ReferenceError);

  const fromParam = new ParamCallsSuper();
  assert.strictEqual(fromParam.tag, 'from-param');
  assert.strictEqual(fromParam.same, true);

  const fromArrow = new ParamArrowCallsSuper();
  assert.strictEqual(fromArrow.tag, 'from-arrow');
  assert.strictEqual(fromArrow.done, true);

  assert.throws(() => new ParamAndBodyCallSuper(), ReferenceError);
  assert.strictEqual(new ParamSkipped('given').tag, 'given');
}
class OptionalKeySuper extends Base { constructor(c) { c?.[super('optional')]; this.x = 1; } }
assert.throws(() => new OptionalKeySuper(null), ReferenceError);
assert.strictEqual(new OptionalKeySuper({}).tag, 'optional');

console.log('ok');
