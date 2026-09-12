import assert from 'node:assert';
import { Buffer } from 'node:buffer';

// Bundlers import generated source as data URLs well beyond filesystem limits.
const source = 'export const meta = import.meta; export default 42;\n' + ' '.repeat(8192);
const urls = [
  `data:text/javascript,${encodeURIComponent(source)}`,
  `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`,
];

for (const url of urls) {
  const module = await import(url);
  assert.strictEqual(module.default, 42);
  assert.strictEqual(module.meta.url, url);
  assert.strictEqual(module.meta.main, false);
  assert.strictEqual(module.meta.env, process.env);
}

console.log('long data URL imports preserve import.meta');
