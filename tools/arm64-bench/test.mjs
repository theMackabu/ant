#!/usr/bin/env node

import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import {
  buildMetadata,
  geometricMean,
  median,
  parseBenchmarkOutput,
  rotate,
  runExecutionId,
  runKey,
  scheduledDate,
  summarize,
  summarizeEngineSamples
} from './lib.mjs';

test('median and dispersion summaries preserve raw scale', () => {
  assert.equal(median([9, 1, 5]), 5);
  assert.equal(median([1, 3, 5, 9]), 4);
  assert.deepEqual(summarize([1, 3, 5]), { median: 3, mad: 2, min: 1, max: 5 });
});

test('geometric mean rejects incomplete scores', () => {
  assert.ok(Math.abs(geometricMean([4, 16]) - 8) < Number.EPSILON * 16);
  assert.equal(geometricMean([1, 0]), null);
  assert.equal(geometricMean([]), null);
});

test('rotation is stable and non-mutating', () => {
  const input = ['a', 'b', 'c'];
  assert.deepEqual(rotate(input, 1), ['b', 'c', 'a']);
  assert.deepEqual(input, ['a', 'b', 'c']);
});

test('benchmark output requires matching finite positive scores', () => {
  assert.deepEqual(parseBenchmarkOutput('result Richards 42\nraw-score 42.5\nscore 42\n', 'Richards'), {
    name: 'Richards',
    score: 42,
    rawScore: 42.5
  });
  assert.throws(() => parseBenchmarkOutput('result Splay 2\nraw-score 2\n', 'Richards'));
  assert.throws(() => parseBenchmarkOutput('result Richards 0\nraw-score 0\n', 'Richards'));
});

test('only complete samples contribute to engine rollups', () => {
  const complete = {
    status: 'ok',
    score: 10,
    results: Object.fromEntries([
      'Richards', 'DeltaBlue', 'Crypto', 'RayTrace', 'EarleyBoyer', 'RegExp', 'Splay', 'NavierStokes'
    ].map(name => [name, 10]))
  };
  const failed = { status: 'failed', score: null, results: { Richards: 1000 } };
  const summary = summarizeEngineSamples([complete, failed]);
  assert.equal(summary.sampleCount, 1);
  assert.equal(summary.score.median, 10);
  assert.equal(summary.results.Richards.median, 10);
});

test('run identity and Pacific scheduled date are deterministic', () => {
  assert.equal(
    runKey('2026-08-29', 'a'.repeat(40), 'b'.repeat(64), 'gh-123-2'),
    'arm64-bench:2026-08-29:aaaaaaaaaaaa:bbbbbbbbbbbb:gh-123-2'
  );
  assert.equal(
    runExecutionId(new Date('2026-08-29T07:30:00.123Z'), {}),
    'local-20260829073000123'
  );
  assert.equal(runExecutionId(new Date(), { GITHUB_RUN_ID: '123', GITHUB_RUN_ATTEMPT: '2' }), 'gh-123-2');
  assert.equal(scheduledDate(new Date('2026-08-29T07:29:00Z')), '2026-08-29');
});

test('build metadata prefers the build-step manifest', async () => {
  const repo = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-build-metadata-'));
  const engineRoot = path.join(repo, 'engine-root');
  const expected = {
    type: 'nix-develop-release-pgo-lto',
    compiler: 'clang version 21',
    tuning: 'native-arm64',
    pgoProfileSha256: 'c'.repeat(64)
  };
  try {
    fs.mkdirSync(path.join(engineRoot, 'ant-bench-build'), { recursive: true });
    fs.writeFileSync(
      path.join(engineRoot, 'ant-bench-build', 'arm64-bench-build.json'),
      JSON.stringify(expected)
    );
    assert.deepEqual(await buildMetadata(repo, engineRoot), expected);
  } finally {
    fs.rmSync(repo, { recursive: true, force: true });
  }
});
