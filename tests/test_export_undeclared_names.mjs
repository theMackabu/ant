import assert from 'node:assert';

const source = 'data:text/javascript,' + encodeURIComponent('export default 5; export let a = 6;');
const load = async (text) => (await import('data:text/javascript,' + encodeURIComponent(text))).default;
const rejects = async (text) => {
  await assert.rejects(() => load(text), SyntaxError, text);
};

await rejects('export {undeclared}; export default 1;');
await rejects('var x; export {x, missing};');
assert.strictEqual(await load('export {x}; let x = 1; export default x;'), 1);
assert.strictEqual(await load('export {f}; function f() {} export default 2;'), 2);
assert.strictEqual(await load(`import {a} from "${source}"; export {a}; export default a;`), 6);

console.log('ok');
