const assert = require('node:assert');

function compiles(src) {
  try { new Function(src); return true } catch (e) {
    assert.ok(e instanceof SyntaxError, src);
    return false;
  }
}

const strictReserved = ['implements', 'interface', 'let', 'package', 'private', 'protected', 'public', 'static', 'yield'];
const lookalikes = [
  'implementz', 'interfaze', 'lot', 'packagf', 'privatf', 'protectee', 'publik', 'statik', 'yielt',
  'awaiu', 'argumentz', 'evam', 'a', 'e', 'i', 'l', 'p', 's', 'y', 'ab', 'is', 'pi', 'left', 'item', 'self',
  'position', 'elements', 'implemented', 'arguments2', 'interfaces', 'packages', 'publics', 'yields', 'lets',
];

const uses = [
  name => `return typeof ${name}`,
  name => `var ${name} = 1; return ${name}`,
  name => `${name}: for (;;) break ${name}`,
  name => `function ${name}() {} return ${name}`,
  name => `return function (${name}) { return ${name} }`,
  name => `return (${name}) => ${name}`,
  name => `return ${name} => ${name}`,
  name => `try {} catch (${name}) {}`,
  name => `var { ${name} } = {}`,
  name => `var [${name}] = []`,
  name => `${name} = 1`,
  name => `${name}++`,
  name => `return { ${name} }`,
];

for (const name of strictReserved) {
  for (const use of uses) {
    assert.strictEqual(compiles(`'use strict'; ${use(name)}`), false, `strict ${use(name)}`);
    if (name === 'let' && use(name).startsWith('let:')) continue;
    assert.strictEqual(compiles(`return function () { ${use(name)} }`), true, `sloppy ${use(name)}`);
    assert.strictEqual(compiles(`class C { m() { ${use(name)} } }`), false, `class ${use(name)}`);
  }
  assert.strictEqual(compiles(`'use strict'; return { ${name}: 1 }.${name}`), true, `property ${name}`);
  const escaped = name[0] + '\\u' + name.charCodeAt(1).toString(16).padStart(4, '0') + name.slice(2);
  assert.strictEqual(compiles(`'use strict'; var ${escaped}`), false, `strict escaped ${name}`);
  assert.strictEqual(compiles(`'use strict'; return typeof ${escaped}`), false, `strict escaped read ${name}`);
  if (name !== 'yield') assert.strictEqual(compiles(`return function* () { var ${escaped}; return ${escaped} }`), true, `sloppy escaped ${name}`);
}

for (const name of lookalikes) {
  for (const use of uses) {
    assert.strictEqual(compiles(`'use strict'; var ${name}; ${use(name)}`), true, `strict ${use(name)}`);
    assert.strictEqual(compiles(`class C { x = function () { var ${name}; ${use(name)} } }`), true, `field ${use(name)}`);
    assert.strictEqual(compiles(`return async function* () { var ${name}; ${use(name)} }`), true, `async generator ${use(name)}`);
  }
}

const bindings = [
  name => `var ${name}`,
  name => `function ${name}() {}`,
  name => `return function (${name}) {}`,
  name => `return (${name}) => 1`,
  name => `return ${name} => 1`,
  name => `try {} catch (${name}) {}`,
  name => `var { ${name} } = {}`,
  name => `var [${name}] = []`,
  name => `${name} = 1`,
  name => `${name}++`,
];

for (const name of ['eval', 'arguments']) {
  for (const bind of bindings) {
    assert.strictEqual(compiles(`'use strict'; ${bind(name)}`), false, `strict ${bind(name)}`);
    assert.strictEqual(compiles(`return function () { ${bind(name)} }`), true, `sloppy ${bind(name)}`);
  }
  assert.strictEqual(compiles(`'use strict'; return typeof ${name}`), true, `strict read ${name}`);
}

assert.strictEqual(compiles('class C { x = arguments }'), false);
assert.strictEqual(compiles('class C { x = () => arguments }'), false);
assert.strictEqual(compiles('class C { static { arguments } }'), false);
assert.strictEqual(compiles('class C { x = function () { return arguments } }'), true);
assert.strictEqual(compiles('class C { x = argument0 }'), true);

assert.strictEqual(compiles('class C { static { await } }'), false);
assert.strictEqual(compiles('class C { static { var await } }'), false);
assert.strictEqual(compiles('class C { static { awaits } }'), true);
assert.strictEqual(compiles('return async function () { var await }'), false);
assert.strictEqual(compiles('return async function () { return await => 1 }'), false);
assert.strictEqual(compiles('return async function () { var awaiu; return awaiu }'), true);
assert.strictEqual(compiles('return function () { var await; return await }'), true);

assert.strictEqual(compiles('return function* () { var yield }'), false);
assert.strictEqual(compiles('return function* () { return yield => 1 }'), false);
assert.strictEqual(compiles('return function* () { yield: 1 }'), false);
assert.strictEqual(compiles('return function* () { var yielt; return yielt }'), true);
assert.strictEqual(compiles('return function () { var yield; return yield }'), true);

console.log('ok');
