import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { discoverTests, flagsFor, loadExpectations, parseArgs } from './runner.mjs';

const options = parseArgs(['--match', 'test-path-', '--limit', '4', '--jobs', '2', '--timeout', '1234']);
assert.equal(options.match.source, 'test-path-');
assert.equal(options.limit, 4);
assert.equal(options.jobs, 2);
assert.equal(options.timeout, 1234);
assert.throws(() => parseArgs(['--jobs', '0']), /positive integer/);
assert.throws(() => parseArgs(['--unknown']), /unknown option/);

assert.deepEqual(flagsFor("// Flags: --expose-gc --title='hello world'\n'use strict';\n"), ['--expose-gc', '--title=hello world']);
assert.deepEqual(flagsFor("'use strict';\n"), []);

const discovered = discoverTests(/parallel\/test-path-/u, 3);
assert.equal(discovered.length, 3);
assert.ok(discovered.every(file => file.startsWith('parallel/test-path-') && file.endsWith('.js')));

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-node-compat-test-'));
try {
  const file = path.join(tempDir, 'expectations.json');
  fs.writeFileSync(
    file,
    JSON.stringify({ version: 1, expectations: [{ pattern: 'parallel/test-http-*.js', expectation: 'skip', reason: 'fixture' }] })
  );
  const expectations = loadExpectations(file);
  assert.equal(expectations.length, 1);
  assert.equal(expectations[0].matcher.test('parallel/test-http-basic.js'), true);
  assert.equal(expectations[0].matcher.test('parallel/test-fs-basic.js'), false);
} finally {
  fs.rmSync(tempDir, { recursive: true, force: true });
}

console.log('node compatibility runner tests passed');
