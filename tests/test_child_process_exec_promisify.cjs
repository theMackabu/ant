const { promisify } = require('util');
const { exec } = require('child_process');

async function main() {
  if (typeof promisify.custom !== 'symbol') {
    throw new Error('expected util.promisify.custom to be a symbol');
  }

  if (typeof exec[promisify.custom] !== 'function') {
    throw new Error('expected exec to define util.promisify.custom');
  }

  const execAsync = promisify(exec);
  const result = await execAsync('printf "OUT"; printf "ERR" >&2');

  if (!result || typeof result !== 'object' || Array.isArray(result)) {
    throw new Error(`expected object result, got ${Object.prototype.toString.call(result)}`);
  }

  if (result.stdout !== 'OUT') {
    throw new Error(`expected stdout to be OUT, got ${JSON.stringify(result.stdout)}`);
  }

  if (result.stderr !== 'ERR') {
    throw new Error(`expected stderr to be ERR, got ${JSON.stringify(result.stderr)}`);
  }

  console.log('util.promisify(exec) resolves { stdout, stderr }');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
