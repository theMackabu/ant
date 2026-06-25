import assert from 'node:assert';
import { createJiti } from 'jiti';

const jiti = createJiti(import.meta.url);
const fixture = await jiti.import('./fixture.ts');

assert.strictEqual(typeof jiti.import, 'function');
assert.strictEqual(fixture.answer, 42);
assert.strictEqual(fixture.message, 'jiti loaded TypeScript through node:vm');

console.log(`${fixture.message}: ${fixture.answer}`);
