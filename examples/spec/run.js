import fs from 'ant:fs';
import path from 'ant:path';
import { $ } from 'ant:shell';

const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const CYAN = '\x1b[36m';
const PINK = '\x1b[35m';
const BOLD = '\x1b[1m';
const DIM = '\x1b[2m';
const RESET = '\x1b[0m';

const specDir = path.dirname(import.meta.url.replace('file://', ''));
const files = fs
  .readdirSync(specDir)
  .filter(f => f.endsWith('.js') && f !== 'run.js' && f !== 'helpers.js')
  .sort();

let totalPassed = 0;
let totalFailed = 0;

let filesPassed = 0;
let filesFailed = 0;

const fileTimes = [];

console.log(`\n${BOLD}${CYAN}Running ${files.length} spec files...${RESET}\n`);

const totalStart = performance.now();

for (const file of files) {
  const filePath = path.join(specDir, file);
  const name = path.basename(file, '.js');

  const start = performance.now();
  try {
    const result = $`./build/ant ${filePath}`;
    const elapsed = performance.now() - start;
    const output = result.text();
    console.log(output);

    const passedMatch = output.match(/Passed:\s*(\d+)/);
    const failedMatch = output.match(/Failed:\s*(\d+)/);

    if (passedMatch) totalPassed += parseInt(passedMatch[1], 10);
    if (failedMatch) totalFailed += parseInt(failedMatch[1], 10);

    if (result.exitCode === 0) {
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
    filesFailed++;
    fileTimes.push({ name, elapsed });
  }
}

const totalElapsed = performance.now() - totalStart;

console.log(`\n${BOLD}Results:${RESET}`);
console.log(`  ${GREEN}${totalPassed} tests passed${RESET}`);
console.log(`  ${RED}${totalFailed} tests failed${RESET}`);
console.log(`  ${GREEN}${filesPassed} files passed${RESET}`);
console.log(`  ${RED}${filesFailed} files failed${RESET}`);

console.log(`\n${BOLD}Timing:${RESET}`);
for (const { name, elapsed } of fileTimes) {
  console.log(`  ${DIM}${name.padEnd(30)}${RESET} ${elapsed.toFixed(0)}ms`);
}
console.log(`\n  ${BOLD}Total${RESET}${' '.padEnd(26)}${totalElapsed.toFixed(0)}ms\n`);
console.log(`${PINK}Welcome to ES6!${RESET}`);

process.exit(totalFailed > 0 ? 1 : 0);
