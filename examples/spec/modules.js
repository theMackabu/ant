import { test, summary } from './helpers.js';

console.log('Module Tests\n');

test('import.meta exists', typeof import.meta, 'object');
test('import.meta.url exists', typeof import.meta.url, 'string');
test('import.meta.url is file', import.meta.url.startsWith('file://'), true);

test('module imported test', typeof test, 'function');
test('module imported summary', typeof summary, 'function');

summary();
