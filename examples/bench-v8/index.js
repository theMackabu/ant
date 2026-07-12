import fs from 'ant:fs';
import os from 'ant:os';
import path from 'ant:path';
import { spawn } from 'child_process';

const benchmarkFiles = [
  'tests/richards.js',
  'tests/deltablue.js',
  'tests/crypto.js',
  'tests/raytrace.js',
  'tests/earley-boyer.js',
  'tests/regexp.js',
  'tests/splay.js',
  'tests/navier-stokes.js'
];

const benchmarkDir = import.meta.dirname;
const repoRoot = path.resolve(benchmarkDir, '..', '..');
const scorePath = path.join(benchmarkDir, 'score.json');
const temporaryDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-bench-v8-'));
const temporaryEntry = path.join(temporaryDir, 'bench-v8.js');

try {
  const base = fs.readFileSync(path.join(benchmarkDir, 'tests/base.js'), 'utf8');
  const harness = fs.readFileSync(path.join(benchmarkDir, 'harness.js'), 'utf8');
  const results = {};
  const rawScores = [];

  for (const benchmarkFile of benchmarkFiles) {
    const source = base + fs.readFileSync(path.join(benchmarkDir, benchmarkFile), 'utf8') + harness;
    fs.writeFileSync(temporaryEntry, source);

    console.log(`running ${benchmarkFile}`);
    const stdout = await runAnt(temporaryEntry);
    const resultMatch = stdout.match(/^result\s+(.+?)\s+(\S+)\s*$/im);
    if (!resultMatch) throw new Error(`${benchmarkFile} did not report a result`);

    const result = Number(resultMatch[2]);
    if (!Number.isFinite(result)) {
      throw new Error(`${benchmarkFile} reported an invalid score: ${resultMatch[2]}`);
    }
    results[resultMatch[1]] = result;

    const rawScoreMatch = stdout.match(/^raw-score\s+(\S+)\s*$/im);
    if (!rawScoreMatch) throw new Error(`${benchmarkFile} did not report a raw score`);

    const rawScore = Number(rawScoreMatch[1]);
    if (!Number.isFinite(rawScore)) {
      throw new Error(`${benchmarkFile} reported an invalid raw score: ${rawScoreMatch[1]}`);
    }
    rawScores.push(rawScore);
  }

  const score = Number(formatScore(geometricMean(rawScores)));
  fs.writeFileSync(scorePath, `${JSON.stringify({ score, results }, null, 2)}\n`);
} finally {
  fs.rmSync(temporaryDir, { recursive: true, force: true });
}

async function runAnt(entry) {
  const child = spawn('./build/ant', [entry], { cwd: repoRoot });
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
  return stdout;
}

function geometricMean(numbers) {
  return Math.exp(numbers.reduce((sum, number) => sum + Math.log(number), 0) / numbers.length);
}

function formatScore(score) {
  return score > 100 ? score.toFixed(0) : score.toPrecision(3);
}
