import fs from 'ant:fs';
import os from 'ant:os';
import path from 'ant:path';
import { spawn } from 'child_process';

const benchmarkFiles = [
  'tests/base.js',
  'tests/richards.js',
  'tests/deltablue.js',
  'tests/crypto.js',
  'tests/raytrace.js',
  'tests/earley-boyer.js',
  'tests/regexp.js',
  'tests/splay.js',
  'tests/navier-stokes.js',
  'harness.js'
];

const benchmarkDir = import.meta.dirname;
const repoRoot = path.resolve(benchmarkDir, '..', '..');
const scorePath = path.join(benchmarkDir, 'score.json');
const temporaryDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-bench-v8-'));
const temporaryEntry = path.join(temporaryDir, 'bench-v8.js');

try {
  const source = benchmarkFiles.map(file => fs.readFileSync(path.join(benchmarkDir, file), 'utf8')).join('');

  fs.writeFileSync(temporaryEntry, source);

  console.log('running bench-v8 with ./build/ant');
  const child = spawn('./build/ant', [temporaryEntry], { cwd: repoRoot });
  let stdout = '';

  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');

  child.stdout.on('data', chunk => {
    stdout += chunk;
    process.stdout.write(chunk);
  });
  child.stderr.on('data', chunk => process.stderr.write(chunk));

  const status = await new Promise((resolve, reject) => {
    child.on('error', reject);
    child.on('close', resolve);
  });

  if (status !== 0) throw new Error(`bench-v8 exited with status ${status}`);

  const scoreMatch = stdout.match(/^score\s+(\S+)\s*$/im);
  if (!scoreMatch) throw new Error('bench-v8 did not report a score');

  const score = Number(scoreMatch[1]);
  if (!Number.isFinite(score)) {
    throw new Error(`bench-v8 reported an invalid score: ${scoreMatch[1]}`);
  }

  const results = {};
  for (const match of stdout.matchAll(/^result\s+(.+?)\s+(\S+)\s*$/gim)) {
    const result = Number(match[2]);
    if (!Number.isFinite(result)) {
      throw new Error(`bench-v8 reported an invalid ${match[1]} score: ${match[2]}`);
    }
    results[match[1]] = result;
  }

  fs.writeFileSync(scorePath, `${JSON.stringify({ score, results }, null, 2)}\n`);
} finally {
  fs.rmSync(temporaryDir, { recursive: true, force: true });
}
