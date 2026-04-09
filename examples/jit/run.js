import fs from 'ant:fs';
import path from 'ant:path';
import { spawnSync } from 'child_process';

const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const CYAN = '\x1b[36m';
const BOLD = '\x1b[1m';
const DIM = '\x1b[2m';
const RESET = '\x1b[0m';

const jitDir = path.dirname(import.meta.url.replace('file://', ''));
const repoRoot = path.resolve(jitDir, '..', '..');
const antBinary = path.join(repoRoot, 'build', 'ant');
const helperFiles = new Set(['run.js']);

const allFiles = fs
  .readdirSync(jitDir)
  .filter(file => file.endsWith('.js') && !helperFiles.has(file))
  .sort();

const cliArgs = process.argv.slice(2);
const runAll = cliArgs.length === 0 || cliArgs.includes('--all');
const requestedFiles = cliArgs.filter(arg => arg !== '--all');

function normalizeFileName(arg) {
  const base = path.basename(arg);
  return base.endsWith('.js') ? base.slice(0, -3) : base;
}

const fileByName = new Map(allFiles.map(file => [path.basename(file, '.js'), file]));
const files = (() => {
  if (runAll) return allFiles;

  const selected = [];
  const seen = new Set();
  const missing = [];

  for (const raw of requestedFiles) {
    const name = normalizeFileName(raw);
    const file = fileByName.get(name);

    if (!file) {
      missing.push(raw);
      continue;
    }

    if (seen.has(file)) continue;
    seen.add(file);
    selected.push(file);
  }

  if (missing.length > 0) {
    console.log(`${RED}Unknown JIT file(s):${RESET} ${missing.join(', ')}`);
  }
  if (selected.length === 0) process.exit(1);

  return selected;
})();

function collectFailureReasons(output, result) {
  const reasons = [];
  const exitCode = result.status ?? result.exitCode;
  const signalCode = result.signal ?? result.signalCode;

  if (exitCode !== 0) reasons.push(`exit code ${exitCode}`);
  if (signalCode !== null && signalCode !== undefined) reasons.push(`signal ${signalCode}`);
  if (/^FAIL\b/m.test(output)) reasons.push('reported FAIL lines');
  if (/\bok:\s*false\b/.test(output)) reasons.push('reported ok: false');
  if (/\bok=false\b/.test(output)) reasons.push('reported ok=false');
  if (/\bresults match:\s*false\b/.test(output)) reasons.push('reported results match: false');
  if (/\ball [^\n]* passed:\s*false\b/i.test(output)) reasons.push('reported final all-passed check as false');

  const failedMatches = [...output.matchAll(/\bfailed:\s*(\d+)\b/gi)];
  for (const match of failedMatches) {
    if (Number(match[1]) > 0) {
      reasons.push(`reported failed count ${match[1]}`);
      break;
    }
  }

  return reasons;
}

function printCaptured(stream, output) {
  if (!output) return;
  stream.write(output);
  if (!output.endsWith('\n')) stream.write('\n');
}

let filesPassed = 0;
let filesFailed = 0;
const fileTimes = [];
const failures = [];

console.log(`\n${BOLD}${CYAN}Running ${files.length} JIT files...${RESET}\n`);
const totalStart = performance.now();

for (const file of files) {
  const relativeFile = path.join('examples', 'jit', file);
  const start = performance.now();

  console.log(`=== Running ${relativeFile} ===`);

  let result;
  try {
    result = spawnSync(antBinary, [relativeFile], { cwd: repoRoot });
  } catch (error) {
    const elapsed = performance.now() - start;
    const message = error && error.stack ? String(error.stack) : String(error);

    console.log(`${RED}✗${RESET} ${path.basename(file, '.js')} ${DIM}(spawn error, ${elapsed.toFixed(0)}ms)${RESET}`);
    console.log(message);
    console.log();

    filesFailed++;
    fileTimes.push({ name: path.basename(file, '.js'), elapsed });
    failures.push({ name: path.basename(file, '.js'), reasons: ['spawn error'] });
    continue;
  }

  printCaptured(process.stdout, result.stdout);
  printCaptured(process.stderr, result.stderr);

  const elapsed = performance.now() - start;
  const name = path.basename(file, '.js');
  const output = `${result.stdout || ''}\n${result.stderr || ''}`;
  const reasons = collectFailureReasons(output, result);

  if (reasons.length === 0) {
    console.log(`${GREEN}✓${RESET} ${name} ${DIM}(${elapsed.toFixed(0)}ms)${RESET}`);
    filesPassed++;
  } else {
    console.log(`${RED}✗${RESET} ${name} ${DIM}(${elapsed.toFixed(0)}ms)${RESET}`);
    console.log(`${DIM}  ${reasons.join('; ')}${RESET}`);
    filesFailed++;
    failures.push({ name, reasons });
  }

  console.log();
  fileTimes.push({ name, elapsed });
}

const totalElapsed = performance.now() - totalStart;

console.log(`${BOLD}Results:${RESET}`);
console.log(`  ${GREEN}${filesPassed} files passed${RESET}`);
console.log(`  ${RED}${filesFailed} files failed${RESET}`);

console.log(`\n${BOLD}Timing:${RESET}`);
for (const { name, elapsed } of fileTimes) {
  console.log(`  ${DIM}${name.padEnd(30)}${RESET} ${elapsed.toFixed(0)}ms`);
}
console.log(`\n  ${BOLD}Total${RESET}${' '.padEnd(26)}${totalElapsed.toFixed(0)}ms\n`);

if (failures.length > 0) {
  console.log(`${BOLD}${RED}Failed Files:${RESET}`);
  for (const { name, reasons } of failures) {
    console.log(`  ${name}: ${reasons.join('; ')}`);
  }
  console.log();
}

process.exit(filesFailed > 0 ? 1 : 0);
