import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn } from 'node:child_process';

const args = process.argv.slice(2);
if (args.includes('--help') || args.length < 2) {
  console.log('Usage: ant examples/demo/bench.js <binary-A> <binary-B> [--only=pi,microbench] [--order=ABBA] [--timeout=180]');
  process.exit(args.includes('--help') ? 0 : 1);
}

const binaries = { A: args[0], B: args[1] };
const options = {};

for (const arg of args.slice(2)) {
  const match = /^--(only|order|timeout)=(.+)$/.exec(arg);
  if (!match) throw new Error(`Unknown option: ${arg}`);
  options[match[1]] = match[2];
}

const order = options.order || 'ABBA';
const timeout = Number(options.timeout || 180) * 1000;
if (!Number.isFinite(timeout) || timeout <= 0) throw new Error('--timeout must be a positive number of seconds');

if (!/^[AB]+$/.test(order) || !order.includes('A') || !order.includes('B')) {
  throw new Error('--order must contain both A and B, e.g. ABBA');
}

const directory = import.meta.dirname;
const selected = options.only ? options.only.split(',') : null;

const files = fs
  .readdirSync(directory)
  .filter(name => {
    if (!/\.(?:[cm]?js|ts)$/.test(name) || name === 'bench.js') return false;
    const source = fs.readFileSync(path.join(directory, name), 'utf8');
    return /performance\.now\s*\(|console\.time\s*\(|Date\.now\s*\(\s*\)\s*-/.test(source);
  })
  .sort();

if (selected) {
  for (const name of selected)
    if (!files.some(file => file === name || file.replace(/\.[^.]+$/, '') === name)) throw new Error(`Unknown timed demo: ${name}`);
}

const demos = files.filter(file => !selected || selected.includes(file) || selected.includes(file.replace(/\.[^.]+$/, '')));
const output = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-demo-bench-'));
const report = { started: new Date().toISOString(), binaries, order, timeout, cwd: process.cwd(), runs: [], summary: [] };

const stamp = text => console.log(`[${new Date().toISOString()}] ${text}`);
const median = values => {
  const sorted = [...values].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 ? sorted[middle] : (sorted[middle - 1] + sorted[middle]) / 2;
};

function measurements(stdout) {
  const result = {};
  for (const line of stdout.replace(/\x1b\[[0-9;]*m/g, '').split('\n')) {
    const match = /^\s*(.+?):\s*(\d+(?:\.\d+)?)\s*(ms|µs\/call|µs|ns|s)\b/.exec(line);
    if (match) result[`${match[1]} (${match[3]})`] = Number(match[2]);
    const rate = /^(\d+(?:\.\d+)?)([MK]?) event loop iterations\/sec/.exec(line);
    if (rate) result['event loop (iterations/sec)'] = Number(rate[1]) * (rate[2] === 'M' ? 1e6 : rate[2] === 'K' ? 1e3 : 1);
  }
  return result;
}

let activeChild = null;
for (const signal of ['SIGINT', 'SIGTERM']) {
  process.once(signal, () => {
    if (activeChild) activeChild.kill('SIGKILL');
    process.exit(signal === 'SIGINT' ? 130 : 143);
  });
}

function run(binary, file) {
  return new Promise(resolve => {
    const start = performance.now();
    const child = spawn(binary, [path.join(directory, file)], { stdio: ['ignore', 'pipe', 'pipe'] });
    activeChild = child;
    let stdout = '',
      stderr = '',
      error = null,
      timedOut = false;
    child.stdout.on('data', data => {
      stdout += data.toString();
    });
    child.stderr.on('data', data => {
      stderr += data.toString();
    });
    child.on('error', err => {
      error = err.message;
    });
    const timer = setTimeout(() => {
      timedOut = true;
      child.kill('SIGKILL');
    }, timeout);
    child.on('close', (code, signal) => {
      clearTimeout(timer);
      activeChild = null;
      resolve({ milliseconds: performance.now() - start, code, signal, timedOut, error, stdout, stderr });
    });
  });
}

stamp(`A: ${binaries.A} | B: ${binaries.B} | ${demos.length} demos | order ${order}`);
console.log(`Logs and JSON: ${output}`);

let failed = false;
for (const file of demos) {
  const runs = [];
  for (let round = 0; round < order.length; round++) {
    const variant = order[round];
    stamp(`${file} ${variant} (${round + 1}/${order.length})`);
    const result = await run(binaries[variant], file);
    const ok = result.code === 0 && !result.error && !result.timedOut;
    const row = { file, round, variant, ...result, ok, measurements: measurements(result.stdout) };
    report.runs.push(row);
    runs.push(row);
    fs.writeFileSync(path.join(output, `${file}.${round}-${variant}.log`), result.stdout + result.stderr);
    console.log(
      `  ${ok ? 'OK' : 'FAIL'} wall ${result.milliseconds.toFixed(3)} ms${ok ? '' : ` (${result.error || (result.timedOut ? 'timeout' : result.signal || `exit ${result.code}`)})`}`
    );
    for (const [label, value] of Object.entries(row.measurements)) console.log(`  ${label}: ${value}`);
    if (!ok) {
      failed = true;
      console.log(result.stderr.slice(-2000));
    }
    fs.writeFileSync(path.join(output, 'results.json'), JSON.stringify(report, null, 2) + '\n');
  }
  if (runs.every(row => row.ok)) {
    for (const metric of ['wall (ms)', ...Object.keys(runs[0].measurements)]) {
      if (metric !== 'wall (ms)' && !runs.every(row => metric in row.measurements)) continue;
      const value = row => (metric === 'wall (ms)' ? row.milliseconds : row.measurements[metric]);
      const a = median(runs.filter(row => row.variant === 'A').map(value));
      const b = median(runs.filter(row => row.variant === 'B').map(value));
      report.summary.push({ file, metric, A: a, B: b, B_minus_A: b - a });
    }
  }
}
console.log('\nMedian comparison (B minus A; lower durations are faster, higher rates are faster):');
for (const row of report.summary) {
  console.log(
    `${row.file} | ${row.metric} | A ${row.A.toFixed(3)} | B ${row.B.toFixed(3)} | Δ ${(row.B_minus_A >= 0 ? '+' : '') + row.B_minus_A.toFixed(3)}`
  );
}

report.finished = new Date().toISOString();
fs.writeFileSync(path.join(output, 'results.json'), JSON.stringify(report, null, 2) + '\n');
stamp(`Done${failed ? ' with failures' : ''}. Results: ${path.join(output, 'results.json')}`);
process.exitCode = failed ? 1 : 0;
