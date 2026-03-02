import fs from 'ant:fs';
import path from 'ant:path';

const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const CYAN = '\x1b[36m';
const BOLD = '\x1b[1m';
const DIM = '\x1b[2m';
const RESET = '\x1b[0m';

const specDir = path.dirname(import.meta.url.replace('file://', ''));
const nonSpecFiles = new Set(['run.js', 'helpers.js', 'import_abs_target.js']);

const allFiles = fs
  .readdirSync(specDir)
  .filter(f => f.endsWith('.js') && !nonSpecFiles.has(f))
  .sort();

const cliArgs = process.argv.slice(2);
const runAll = cliArgs.includes('--all');
const requestedSpecs = cliArgs.filter(arg => arg !== '--all');

function normalizeSpecName(arg) {
  const base = path.basename(arg);
  return base.endsWith('.js') ? base.slice(0, -3) : base;
}

const fileByName = new Map(allFiles.map(file => [path.basename(file, '.js'), file]));
const files = (() => {
  if (!runAll && requestedSpecs.length === 0) {
    console.log(`${RED}No specs selected.${RESET} Use ${BOLD}--all${RESET} or pass one or more spec names.`);
    process.exit(1);
  }
  if (runAll) return allFiles;

  const selected = [];
  const seen = new Set();
  const missing = [];

  for (const raw of requestedSpecs) {
    const name = normalizeSpecName(raw);
    const file = fileByName.get(name);

    if (!file) {
      missing.push(raw);
      continue;
    }

    if (seen.has(file)) continue;
    seen.add(file);
    selected.push(file);
  }

  if (missing.length > 0) console.log(`${RED}Unknown spec file(s):${RESET} ${missing.join(', ')}`);
  if (selected.length === 0) process.exit(1);

  return selected;
})();

let totalPassed = 0;
let totalFailed = 0;

let filesPassed = 0;
let filesFailed = 0;

const fileTimes = [];
const failedTests = [];

console.log(`\n${BOLD}${CYAN}Running ${files.length} spec files...${RESET}\n`);
const totalStart = performance.now();

async function runInProcessMode() {
  const hook = {
    current: null,
    onSummary: null
  };
  globalThis.__ANT_SPEC_HOOK__ = hook;

  for (let i = 0; i < files.length; i++) {
    const file = files[i];
    const name = path.basename(file, '.js');
    const fileUrl = `./${file}`;
    const start = performance.now();

    hook.current = { passed: 0, failed: 0, failures: [] };
    let summaryResolve;
    const summaryP = new Promise(resolve => {
      summaryResolve = resolve;
    });
    hook.onSummary = summaryResolve;

    try {
      await import(fileUrl);
      const result = await summaryP;
      const elapsed = performance.now() - start;

      totalPassed += result.passed;
      totalFailed += result.failed;
      if (result.failures.length > 0) {
        failedTests.push({ name, failures: result.failures });
      }

      if (result.failed === 0) {
        console.log(`${GREEN}✓${RESET} ${name} ${DIM}(${elapsed.toFixed(0)}ms)${RESET}\n`);
        filesPassed++;
      } else {
        console.log(`${RED}✗${RESET} ${name} ${DIM}(${elapsed.toFixed(0)}ms)${RESET}\n`);
        filesFailed++;
      }
      fileTimes.push({ name, elapsed });
    } catch (e) {
      const elapsed = performance.now() - start;
      console.log(`${RED}✗${RESET} ${name} ${DIM}(error, ${elapsed.toFixed(0)}ms)${RESET}\n`);
      if (e && e.stack) console.log(String(e.stack));
      else console.log(String(e));
      filesFailed++;
      totalFailed++;
      failedTests.push({ name, failures: [`${RED}✗${RESET} import/exec error`] });
      fileTimes.push({ name, elapsed });
    } finally {
      hook.onSummary = null;
      hook.current = null;
    }
  }
}

await runInProcessMode();
const totalElapsed = performance.now() - totalStart;

console.log(`\n${BOLD}Results:${RESET}`);
console.log(`  ${GREEN}${totalPassed} tests passed${RESET}`);
console.log(`  ${RED}${totalFailed} tests failed${RESET}`);
console.log(`  ${GREEN}${filesPassed} files passed${RESET}`);
console.log(`  ${RED}${filesFailed} files failed${RESET}`);

console.log(`\n${BOLD}Timing:${RESET}`);
for (const { name, elapsed } of fileTimes) console.log(`  ${DIM}${name.padEnd(30)}${RESET} ${elapsed.toFixed(0)}ms`);
console.log(`\n  ${BOLD}Total${RESET}${' '.padEnd(26)}${totalElapsed.toFixed(0)}ms\n`);

if (failedTests.length > 0) {
  console.log(`${BOLD}${RED}Failed Tests:${RESET}`);
  for (const { name, failures } of failedTests) {
    console.log(`\n  ${BOLD}${name}${RESET}`);
    for (const line of failures) console.log(`    ${line.trim()}`);
  }
  console.log();
}

process.exit(totalFailed > 0 ? 1 : 0);
