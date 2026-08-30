#!/usr/bin/env node

import childProcess from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, '../..');
const suiteRoot = path.join(repoRoot, 'vendor/node-compat/test');
const parallelRoot = path.join(suiteRoot, 'parallel');
const defaultExpectations = path.join(here, 'expectations.json');
const bootstrap = path.join(here, 'bootstrap.cjs');

function usage() {
  return `Usage: node tests/node-compat/runner.mjs [options]

Run Bun's pinned copy of the Node.js parallel test suite under Ant.

Options:
  --ant PATH             Ant executable (default: ./build/ant)
  --match REGEXP         Select paths matching a JavaScript regular expression
  --limit N              Run at most N selected tests
  --jobs N               Concurrent processes (default: up to 8 CPUs)
  --timeout MS           Per-test timeout (default: 10000)
  --expectations PATH    Expectations file
  --json PATH            Write the complete JSON report to PATH
  --list                 List selected tests without running them
  --verbose              Print captured output for failures
  --help                 Show this help
`;
}

export function parseArgs(argv) {
  const options = {
    ant: path.join(repoRoot, 'build/ant'),
    match: null,
    limit: Infinity,
    jobs: Math.max(1, Math.min(8, os.availableParallelism?.() || os.cpus().length || 1)),
    timeout: 10000,
    expectations: defaultExpectations,
    json: null,
    list: false,
    verbose: false,
    help: false
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    const take = () => {
      const value = argv[++index];
      if (value === undefined) throw new Error(`${argument} requires a value`);
      return value;
    };
    if (argument === '--ant') options.ant = path.resolve(take());
    else if (argument === '--match') options.match = new RegExp(take(), 'u');
    else if (argument === '--limit') options.limit = Number(take());
    else if (argument === '--jobs') options.jobs = Number(take());
    else if (argument === '--timeout') options.timeout = Number(take());
    else if (argument === '--expectations') options.expectations = path.resolve(take());
    else if (argument === '--json') options.json = path.resolve(take());
    else if (argument === '--list') options.list = true;
    else if (argument === '--verbose') options.verbose = true;
    else if (argument === '--help' || argument === '-h') options.help = true;
    else throw new Error(`unknown option: ${argument}`);
  }
  for (const [name, value] of [
    ['limit', options.limit],
    ['jobs', options.jobs],
    ['timeout', options.timeout]
  ]) {
    if (value !== Infinity && (!Number.isSafeInteger(value) || value < 1)) throw new Error(`--${name} must be a positive integer`);
  }
  return options;
}

function globToRegExp(glob) {
  let source = '^';
  for (let index = 0; index < glob.length; index += 1) {
    const character = glob[index];
    if (character === '*' && glob[index + 1] === '*') {
      source += '.*';
      index += 1;
    } else if (character === '*') source += '[^/]*';
    else if (character === '?') source += '[^/]';
    else source += character.replace(/[\\^$.*+?()[\]{}|]/g, '\\$&');
  }
  return new RegExp(`${source}$`, 'u');
}

export function loadExpectations(file) {
  const document = JSON.parse(fs.readFileSync(file, 'utf8'));
  if (document.version !== 1 || !Array.isArray(document.expectations)) throw new Error('unsupported expectations format');
  return document.expectations.map((entry, index) => {
    if (!['fail', 'skip'].includes(entry.expectation)) throw new Error(`expectation ${index} must be fail or skip`);
    if (typeof entry.pattern !== 'string' || typeof entry.reason !== 'string') throw new Error(`expectation ${index} needs pattern and reason`);
    return { ...entry, matcher: globToRegExp(entry.pattern) };
  });
}

function expectationFor(relativePath, expectations) {
  return expectations.find(expectation => expectation.matcher.test(relativePath)) || null;
}

function shellWords(value) {
  const words = [];
  let word = '';
  let quote = null;
  let escaped = false;
  for (const character of value) {
    if (escaped) {
      word += character;
      escaped = false;
    } else if (character === '\\' && quote !== "'") escaped = true;
    else if (quote) {
      if (character === quote) quote = null;
      else word += character;
    } else if (character === "'" || character === '"') quote = character;
    else if (/\s/u.test(character)) {
      if (word) {
        words.push(word);
        word = '';
      }
    } else word += character;
  }
  if (escaped) word += '\\';
  if (quote) throw new Error('unterminated quote in Flags directive');
  if (word) words.push(word);
  return words;
}

export function flagsFor(source) {
  const header = source.split(/\r?\n/u).slice(0, 40).join('\n');
  const match = header.match(/^\/\/ Flags:\s*(.+)$/mu);
  return match ? shellWords(match[1]) : [];
}

export function discoverTests(match, limit) {
  const tests = fs
    .readdirSync(parallelRoot, { withFileTypes: true })
    .filter(entry => entry.isFile() && /^test-.+\.js$/u.test(entry.name))
    .map(entry => `parallel/${entry.name}`)
    .filter(relativePath => !match || match.test(relativePath))
    .sort();
  return tests.slice(0, limit);
}

function appendLimited(state, chunk) {
  if (state.length >= 1024 * 1024) return state;
  return state + chunk.slice(0, 1024 * 1024 - state.length);
}

export function runOne(relativePath, options) {
  return new Promise(resolve => {
    const file = path.join(suiteRoot, relativePath);
    const flags = flagsFor(fs.readFileSync(file, 'utf8'));
    const started = performance.now();
    const child = childProcess.spawn(options.ant, [...flags, bootstrap, file], {
      cwd: repoRoot,
      detached: process.platform !== 'win32',
      env: {
        ...process.env,
        NODE_TEST_DIR: suiteRoot,
        NODE_TEST_CONTEXT: 'child-v8'
      },
      stdio: ['ignore', 'pipe', 'pipe']
    });
    let stdout = '';
    let stderr = '';
    let timedOut = false;
    const killTree = () => {
      try {
        if (process.platform === 'win32') child.kill('SIGKILL');
        else process.kill(-child.pid, 'SIGKILL');
      } catch {
        try {
          child.kill('SIGKILL');
        } catch {}
      }
    };
    child.stdout.on('data', chunk => {
      stdout = appendLimited(stdout, String(chunk));
    });
    child.stderr.on('data', chunk => {
      stderr = appendLimited(stderr, String(chunk));
    });
    const timer = setTimeout(() => {
      timedOut = true;
      killTree();
    }, options.timeout);
    child.on('error', error => {
      clearTimeout(timer);
      resolve({ path: relativePath, flags, outcome: 'spawn-error', status: null, signal: null, durationMs: performance.now() - started, stdout, stderr, error: error.message });
    });
    child.on('exit', (status, signal) => {
      clearTimeout(timer);
      killTree();
      resolve({
        path: relativePath,
        flags,
        outcome: timedOut ? 'timeout' : signal ? 'crash' : status === 0 ? 'pass' : 'fail',
        status,
        signal,
        durationMs: performance.now() - started,
        stdout,
        stderr
      });
    });
  });
}

async function runPool(tests, options, onResult) {
  let next = 0;
  async function worker() {
    for (;;) {
      const index = next++;
      if (index >= tests.length) return;
      onResult(await runOne(tests[index], options));
    }
  }
  await Promise.all(Array.from({ length: Math.min(options.jobs, tests.length) }, worker));
}

function classify(result, expectation) {
  if (result.outcome === 'pass') return expectation?.expectation === 'fail' ? 'unexpected-pass' : 'pass';
  return expectation?.expectation === 'fail' ? 'expected-fail' : 'unexpected-fail';
}

function printFailure(result, verbose) {
  console.log(`${result.classification.toUpperCase()} ${result.path} (${result.outcome}, ${Math.round(result.durationMs)}ms)`);
  if (result.flags.length) console.log(`  flags: ${result.flags.join(' ')}`);
  if (verbose) {
    if (result.stdout) console.log(`  stdout:\n${result.stdout.trimEnd()}`);
    if (result.stderr) console.log(`  stderr:\n${result.stderr.trimEnd()}`);
  }
}

export async function main(argv = process.argv.slice(2)) {
  let options;
  try {
    options = parseArgs(argv);
  } catch (error) {
    console.error(error.message);
    console.error(usage());
    return 2;
  }
  if (options.help) {
    console.log(usage());
    return 0;
  }
  if (!fs.existsSync(parallelRoot)) {
    console.error(`vendored suite not found: ${parallelRoot}`);
    return 2;
  }
  if (!options.list && !fs.existsSync(options.ant)) {
    console.error(`Ant executable not found: ${options.ant}`);
    return 2;
  }
  let expectations;
  try {
    expectations = loadExpectations(options.expectations);
  } catch (error) {
    console.error(`cannot load expectations: ${error.message}`);
    return 2;
  }
  const selected = discoverTests(options.match, options.limit);
  if (options.list) {
    for (const test of selected) console.log(test);
    return 0;
  }

  const skipped = [];
  const runnable = [];
  for (const test of selected) {
    const expectation = expectationFor(test, expectations);
    if (expectation?.expectation === 'skip') skipped.push({ path: test, reason: expectation.reason });
    else runnable.push(test);
  }
  const results = [];
  await runPool(runnable, options, result => {
    const expectation = expectationFor(result.path, expectations);
    result.classification = classify(result, expectation);
    result.expectation = expectation ? { expectation: expectation.expectation, reason: expectation.reason } : null;
    results.push(result);
    if (result.classification !== 'pass' && result.classification !== 'expected-fail') printFailure(result, options.verbose);
  });
  results.sort((left, right) => left.path.localeCompare(right.path));

  const counts = { pass: 0, 'expected-fail': 0, 'unexpected-fail': 0, 'unexpected-pass': 0, skip: skipped.length };
  for (const result of results) counts[result.classification] += 1;
  const report = {
    generatedAt: new Date().toISOString(),
    ant: options.ant,
    suite: { source: 'oven-sh/bun', commit: 'ed950b88ab2ec6b58bccdfe7d310731b8ca13c4d', group: 'parallel' },
    options: { match: options.match?.source || null, limit: options.limit, jobs: options.jobs, timeout: options.timeout },
    counts,
    skipped,
    results
  };
  if (options.json) {
    fs.mkdirSync(path.dirname(options.json), { recursive: true });
    fs.writeFileSync(options.json, `${JSON.stringify(report, null, 2)}\n`);
  }
  console.log(`\n${selected.length} selected: ${counts.pass} pass, ${counts['expected-fail']} expected fail, ${counts['unexpected-fail']} unexpected fail, ${counts['unexpected-pass']} unexpected pass, ${counts.skip} skipped`);
  return counts['unexpected-fail'] || counts['unexpected-pass'] ? 1 : 0;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) process.exitCode = await main();
