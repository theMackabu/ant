const { promisify } = require('node:util');
const { execFile } = require('node:child_process');
const path = require('node:path');

async function main() {
  const execFileAsync = promisify(execFile);
  const cwd = path.join(__dirname, '..');

  const revParse = await execFileAsync(
    'git',
    ['rev-parse', '--show-toplevel'],
    { cwd, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 }
  );

  if (!revParse || typeof revParse.stdout !== 'string') {
    throw new Error('expected rev-parse stdout string');
  }
  if (!revParse.stdout.trim().endsWith('/ant')) {
    throw new Error(`unexpected rev-parse output: ${JSON.stringify(revParse.stdout)}`);
  }

  const status = await execFileAsync(
    'git',
    ['status', '--porcelain=v1', '-z', '--untracked-files=normal'],
    { cwd, encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 }
  );

  if (!status || typeof status.stdout !== 'string') {
    throw new Error('expected status stdout string');
  }

  console.log('util.promisify(execFile) handles git with options');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
