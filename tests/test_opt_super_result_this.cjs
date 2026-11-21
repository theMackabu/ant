const assert = require('node:assert');

class ReturnsOther { constructor(kind) { if (kind === 'other') return { other: true }; if (kind === 'proxy') return new Proxy(this, {}); this.own = kind; } }

class CatchAfterSuper extends ReturnsOther {
  constructor(kind, fail) {
    super(kind);
    try { if (fail) throw new Error('late'); this.ok = true; } catch (e) { this.caught = e.message; }
  }
}

class CatchAfterBranchySuper extends ReturnsOther {
  constructor(kind, fail) {
    if (fail) { super(kind); } else { super(kind); }
    try { if (fail) throw new Error('late'); this.ok = true; } catch (e) { this.caught = e.message; }
    this.after = true;
  }
}

class FinallyAfterSuper extends ReturnsOther {
  constructor(kind, fail) {
    super(kind);
    try { if (fail) throw new Error('late'); } catch { this.caught = true; } finally { this.done = true; }
  }
}

for (let i = 0; i < 40000; i++) {
  const fail = i % 1000 === 999;
  for (const Class of [CatchAfterSuper, CatchAfterBranchySuper, FinallyAfterSuper]) {
    const replaced = new Class('other', fail);
    if (replaced.other !== true || replaced instanceof Class) assert.fail(`${Class.name} lost the replaced receiver at ${i}`);
    if (fail && !replaced.caught) assert.fail(`${Class.name} lost the catch write at ${i}`);
    if (Class === CatchAfterBranchySuper && replaced.after !== true) assert.fail(`${Class.name} lost the write after catch at ${i}`);
    if (Class === FinallyAfterSuper && replaced.done !== true) assert.fail(`${Class.name} lost the finally write at ${i}`);
  }
  if (fail) {
    const proxied = new CatchAfterBranchySuper('proxy', true);
    assert.strictEqual(proxied.caught, 'late');
    assert.strictEqual(proxied.after, true);
  }
}

console.log('ok');
