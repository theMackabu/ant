const assert = require('node:assert');
const { spawnSync } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const invalid = [
  'let value = new.target;',
  'new.target;',
  'if (false) { new.target; }',
  'false && new.target;',
  '() => new.target;',
  '(value = new.target) => value;',
  '({ [new.target]: 1 });',
  'class C extends (new.target || Object) {}',
  'class C { [new.target]() {} }',
  'class C { [new.target] = 1; }',
  'class C { static [new.target] = 1; }',
];

// Indirect eval always parses global code, even during construction.
function checkIndirect() {
  const indirect = eval;
  for (const source of invalid) {
    globalThis.newTargetSyntaxRan = false;
    assert.throws(() => indirect('globalThis.newTargetSyntaxRan = true; ' + source), SyntaxError, source);
    assert.strictEqual(globalThis.newTargetSyntaxRan, false, 'must reject before executing');
  }
  assert.throws(() => (0, eval)('new.target'), SyntaxError);
  assert.throws(() => eval.call(null, 'new.target'), SyntaxError);
  assert.throws(() => indirect('eval("new.target")'), SyntaxError);
  assert.throws(() => indirect('(() => eval("new.target"))()'), SyntaxError);
}
checkIndirect();
new checkIndirect();
delete globalThis.newTargetSyntaxRan;

// Direct eval inherits syntax permission, independently of the target's value.
function direct() {
  new.target;
  const source = "new.target";
  assert.strictEqual(eval(source), new.target);
  assert.strictEqual(eval('new.target'), new.target);
  assert.strictEqual(eval('eval("new.target")'), new.target);
  assert.strictEqual((() => eval('new.target'))(), new.target);
  assert.strictEqual(eval('() => new.target')(), new.target);
}
direct();
new direct();
assert.strictEqual(Function('return eval("new.target")')(), undefined);
const dynamic = Function('this.target = eval("new.target")');
assert.strictEqual(new dynamic().target, dynamic);

// Class initialization establishes a function context even at global scope.
const validGlobal = `
  const source = 'new.target';
  function f() { new.target; return eval(source); }
  if (f() !== undefined) throw new Error('ordinary function');
  class C {
    field = eval(source);
    arrow = () => eval(source);
    static field = eval(source);
    static { if (eval(source) !== undefined) throw new Error('static block'); }
  }
  const c = new C();
  if (c.field !== undefined || c.arrow() !== undefined || C.field !== undefined)
    throw new Error('class initialization');
`;
(0, eval)(validGlobal);

// A CommonJS wrapper is a function: use actual script/module entries here.
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-new-target-syntax-'));
try {
  for (const source of [...invalid, 'eval("new.target")', '(() => eval("new.target"))()']) {
    for (const mode of ['script', 'module']) {
      const file = path.join(dir, 'entry.mjs');
      fs.writeFileSync(file, source);
      const args = mode === 'script' ? [] : [file];
      const result = spawnSync(process.execPath, args, { input: source, encoding: 'utf8' });
      if (result.error) throw result.error;
      assert.notStrictEqual(result.status, 0, `${mode}: ${source}`);
      assert.match(result.stderr, /SyntaxError/, `${mode}: ${source}\n${result.stderr}`);
    }
  }
  const result = spawnSync(process.execPath, [], { input: validGlobal, encoding: 'utf8' });
  if (result.error) throw result.error;
  assert.strictEqual(result.status, 0, result.stderr);
} finally {
  fs.rmSync(dir, { recursive: true, force: true });
}
console.log('new-target:syntax:ok');
