import fs from 'ant:fs';
import path from 'ant:path';
import { $ } from 'ant:shell';

const GREEN = '\x1b[32m';
const RED = '\x1b[31m';
const CYAN = '\x1b[36m';
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

console.log(`\n${BOLD}${CYAN}Running ${files.length} spec files...${RESET}\n`);

for (const file of files) {
  const filePath = path.join(specDir, file);
  const name = path.basename(file, '.js');

  try {
    const result = await $`./build/ant ${filePath}`;
    const output = result.text();
    console.log(output);

    const passedMatch = output.match(/Passed:\s*(\d+)/);
    const failedMatch = output.match(/Failed:\s*(\d+)/);

    if (passedMatch) totalPassed += parseInt(passedMatch[1], 10);
    if (failedMatch) totalFailed += parseInt(failedMatch[1], 10);

    if (result.exitCode === 0) {
      console.log(`${GREEN}✓${RESET} ${name}`);
      filesPassed++;
    } else {
      console.log(`${RED}✗${RESET} ${name}`);
      filesFailed++;
    }
  } catch (e) {
    console.log(`${RED}✗${RESET} ${name} ${DIM}(error)${RESET}`);
    filesFailed++;
  }
}

console.log(`\n${BOLD}Results:${RESET}`);
console.log(`  ${GREEN}${totalPassed} tests passed${RESET}`);
console.log(`  ${RED}${totalFailed} tests failed${RESET}`);
console.log(`  ${GREEN}${filesPassed} files passed${RESET}`);
console.log(`  ${RED}${filesFailed} files failed${RESET}\n`);

process.exit(failed > 0 ? 1 : 0);
