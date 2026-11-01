// Node-side checks for the differential runner:
//   node tests/differential/runner.test.ts

import fc from 'fast-check';
import assert from 'node:assert/strict';
import childProcess from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { test } from 'node:test';
import { differingLabels, repoRoot, runEngine, sameResult, sameVariantResult, type EngineResult } from './engine.ts';
import { familyNamed, familyNames, families } from './families/index.ts';
import { parseArgs } from './runner.ts';

test('family registry', () => {
  assert.deepEqual(familyNames, [
    'property', 'regexp', 'promise', 'stream-shape', 'path',
    'json', 'number', 'string', 'structured-clone', 'textcodec', 'url', 'buffer-base64', 'jit'
  ]);
  assert.throws(() => familyNamed('missing'), /unknown family/);
  assert.deepEqual(familyNamed('jit').variants?.map(variant => variant.name), ['default', 'jitless', 'opt:off', 'opt:force', 'opt:verify']);
});

test('generation is deterministic', () => {
  for (const family of families) {
    const build = (seed: number) => fc.sample(family.arbitrary, { seed, numRuns: 3 }).map(params => family.build(params));
    assert.deepEqual(build(42), build(42), `${family.name} must be deterministic for a seed`);
  }
});

test('every generated probe parses', () => {
  for (const family of families) {
    fc.assert(
      fc.property(family.arbitrary, params => {
        const source = family.build(params);
        assert.match(source, /console\.log\(JSON\.stringify\(__out\)\)/);
        new Function(source);
      }),
      { seed: 7, numRuns: family.name === 'path' ? 20 : 200 }
    );
  }
});

test('shrinking reaches small kernels', () => {
  // a property failing on any kernel with a statement shrinks to one kernel
  // with one statement
  type Params = { kernels: { statements: string[] }[] };
  const jit = familyNamed('jit');
  const details = fc.check(
    fc.property(jit.arbitrary, (params: Params) => params.kernels.every(kernel => kernel.statements.length === 0)),
    { seed: 3, numRuns: 200 }
  );
  assert.ok(details.failed && details.counterexample);
  const [shrunk] = details.counterexample as [Params];
  assert.equal(shrunk.kernels.length, 1);
  assert.equal(shrunk.kernels[0].statements.length, 1);
});

test('argument parsing', () => {
  const parsed = parseArgs(['--family', 'regexp', '--cases', '3', '--seed', '9', '--timeout', '25', '--minimize', '--json']);
  assert.deepEqual(parsed.families, ['regexp']);
  assert.equal(parsed.casesPerFamily, 3);
  assert.equal(parsed.seed, 9);
  assert.equal(parsed.timeout, 25);
  assert.equal(parsed.minimize, true);
  assert.equal(parsed.json, true);
  assert.throws(() => parseArgs(['--family', 'missing']), /unknown family/);
  assert.throws(() => parseArgs(['--cases', '0']), /valid integer/);
});

const result = (stdout: string, stderr = ''): EngineResult => ({ status: 0, signal: null, timedOut: false, stdout, stderr, spawnError: null });

test('result comparison', () => {
  assert.equal(sameResult({ stdout: 'x' }, { stdout: 'x' }), true);
  assert.equal(sameResult({ stdout: 'x' }, { stdout: 'y' }), false);
  const traced = { name: 'traced', stderrFailures: 'codegen-failed' };
  assert.equal(sameVariantResult(result('x'), result('x', 'jit: opt compiled'), traced), true);
  assert.equal(sameVariantResult(result('x'), result('x', 'codegen-failed func=f'), traced), false);
  assert.equal(sameVariantResult(result('x'), result('y'), traced), false);
  assert.deepEqual(differingLabels(result('[["a",1],["b",2]]'), result('[["a",1],["b",3]]')), ['b']);
  assert.deepEqual(differingLabels(result('not json'), result('other')), ['<stdout>']);
});

test('engine runs pass variant arguments and environment', () => {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-differential-test-'));
  try {
    const probe = path.join(tempDir, 'env.cjs');
    fs.writeFileSync(probe, "console.log(process.env.PROBE_VARIANT, process.env.ANT_DEBUG ?? 'unset', process.execArgv.join(' '))\n");
    process.env.ANT_DEBUG = 'leaked';
    assert.equal(runEngine(process.execPath, probe, 5000).stdout, 'undefined unset \n', 'ANT_DEBUG must not leak into probes');
    const variant = { name: 'v', args: ['--no-warnings'], env: { PROBE_VARIANT: 'x', ANT_DEBUG: 'opt:off' } };
    assert.equal(runEngine(process.execPath, probe, 5000, variant).stdout, 'x opt:off --no-warnings\n');
  } finally {
    delete process.env.ANT_DEBUG;
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
});

test('the runner reports equality when both engines are Node', () => {
  const run = childProcess.spawnSync(
    process.execPath,
    ['tests/differential/runner.ts', '--node', process.execPath, '--ant', process.execPath, '--family', 'property', '--family', 'json', '--cases', '3', '--json'],
    { cwd: repoRoot, encoding: 'utf8' }
  );
  assert.equal(run.status, 0, run.stderr);
  const records = run.stdout.trim().split('\n').map(line => JSON.parse(line));
  assert.deepEqual(records.map(record => [record.family, record.match, record.runs]), [['property', true, 3], ['json', true, 3]]);
});
