import assert from 'node:assert';

const source = 'data:text/javascript,' + encodeURIComponent('export default 5; export let a = 6;');
const load = async (text) => (await import('data:text/javascript,' + encodeURIComponent(text))).default;
const rejects = async (text) => {
  await assert.rejects(() => load(text), SyntaxError, text);
};

for (const name of ['undefined', 'window', 'globalThis', 'NaN', 'async', 'of', 'as', 'from', 'using']) {
  assert.strictEqual(await load(`import ${name} from "${source}"; export default ${name};`), 5, name);
  assert.strictEqual(await load(`import {a as ${name}} from "${source}"; export default ${name};`), 6, name);
  assert.strictEqual(await load(`import * as ${name} from "${source}"; export default typeof ${name};`), 'object', name);
  assert.strictEqual(await load(`import ${name}, {a} from "${source}"; export default ${name} + a;`), 11, name);
  assert.strictEqual(await load(`import {default as ${name}} from "${source}"; export default ${name};`), 5, name);
}

for (const name of ['eval', 'arguments', 'let', 'static', 'yield', 'await', 'implements', 'interface', 'package', 'private', 'protected', 'public', 'default', 'var', 'enum', 'null', 'true', 'this']) {
  await rejects(`import ${name} from "${source}"; export default 1;`);
  await rejects(`import {a as ${name}} from "${source}"; export default 1;`);
  await rejects(`import * as ${name} from "${source}"; export default 1;`);
}

console.log('ok');
