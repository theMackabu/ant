const { spawnSync } = require('child_process');
const path = require('path');

function run(file, rounds) {
  const result = spawnSync(process.execPath, [file, String(rounds)], {
    encoding: 'utf8',
  });
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(
      `${path.basename(file)} exited ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
    );
  }

  const out = Object.create(null);
  for (const line of result.stdout.trim().split(/\n/)) {
    const eq = line.indexOf('=');
    if (eq > 0) out[line.slice(0, eq)] = line.slice(eq + 1);
  }
  return {
    ms: Number(out.best_ms),
    checksum: out.checksum,
    stdout: result.stdout,
  };
}

const rounds = Number(process.argv[2]) > 0 ? Number(process.argv[2]) | 0 : 5000000;
const dir = __dirname;
const ts = run(path.join(dir, 'bench_typescript_type_hints_compare.ts'), rounds);
const js = run(path.join(dir, 'bench_typescript_type_hints_compare.js'), rounds);

if (ts.checksum !== js.checksum) {
  throw new Error(`checksum mismatch: ts=${ts.checksum} js=${js.checksum}`);
}

const speedup = js.ms / ts.ms;
const delta = js.ms - ts.ms;

console.log(`rounds=${rounds}`);
console.log(`ts_best_ms=${ts.ms.toFixed(3)}`);
console.log(`js_best_ms=${js.ms.toFixed(3)}`);
console.log(`speedup=${speedup.toFixed(2)}x`);
console.log(`delta_ms=${delta.toFixed(3)}`);
console.log(`checksum=${ts.checksum}`);
