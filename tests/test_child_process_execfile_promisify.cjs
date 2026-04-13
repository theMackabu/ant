const { promisify } = require('util');
const { execFile } = require('child_process');
const path = require('path');

async function main() {
  if (typeof promisify.custom !== 'symbol') {
    throw new Error('expected util.promisify.custom to be a symbol');
  }

  if (typeof execFile[promisify.custom] !== 'function') {
    throw new Error('expected execFile to define util.promisify.custom');
  }

  const execFileAsync = promisify(execFile);
  const child = path.join(__dirname, 'fixtures', 'execfile_promisify_child.cjs');
  const result = await execFileAsync(process.execPath, [child]);

  if (!result || typeof result !== 'object' || Array.isArray(result)) {
    throw new Error(`expected object result, got ${Object.prototype.toString.call(result)}`);
  }

  if (result.stdout !== 'OUT') {
    throw new Error(`expected stdout to be OUT, got ${JSON.stringify(result.stdout)}`);
  }

  if (result.stderr !== 'ERR') {
    throw new Error(`expected stderr to be ERR, got ${JSON.stringify(result.stderr)}`);
  }

  console.log('util.promisify(execFile) resolves { stdout, stderr }');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
