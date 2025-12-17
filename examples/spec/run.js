import fs from 'fs';
import path from 'path';
import { spawn } from 'child_process';

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

let passed = 0;
let failed = 0;

console.log(`\n${BOLD}${CYAN}Running ${files.length} spec files...${RESET}\n`);

for (const file of files) {
  const filePath = path.join(specDir, file);
  const name = path.basename(file, '.js');

  try {
    const result = await new Promise((resolve, reject) => {
      const proc = spawn('ant', [filePath], { timeout: 30000 });
      let stdout = '';
      let stderr = '';
      proc.stdout.on('data', d => (stdout += d));
      proc.stderr.on('data', d => (stderr += d));
      proc.on('close', code => resolve({ code, stdout, stderr }));
      proc.on('error', reject);
    });

    if (result.code === 0) {
      console.log(`${GREEN}✓${RESET} ${name}`);
      passed++;
    } else {
      console.log(`${RED}✗${RESET} ${name}`);
      failed++;
    }
  } catch (e) {
    console.log(`${RED}✗${RESET} ${name} ${DIM}(error)${RESET}`);
    failed++;
  }
}

console.log(`\n${BOLD}Results:${RESET}`);
console.log(`  ${GREEN}${passed} passed${RESET}`);
console.log(`  ${RED}${failed} failed${RESET}\n`);

process.exit(failed > 0 ? 1 : 0);
