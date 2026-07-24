import { spawn } from 'node:child_process';
import { existsSync, readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const ANT = resolve(process.env.ANT_TEST_BIN || process.execPath);
const ERROR_MARKERS = /TypeError|ReferenceError|RangeError|Invalid |SIGBUS|panic|FATAL/;

export const defaults = {
  requests: 600,
  concurrency: 8,
  readyTimeoutMs: 8000,
  targetTimeoutMs: 60000
};

function now() {
  return performance.now();
}

function fetchT(url, ms) {
  return fetch(url, { signal: AbortSignal.timeout(ms) });
}

function npmExampleDir(entry) {
  return entry.match(/^(examples\/npm\/[^/]+)/)?.[1] ?? null;
}

function needsInstall(dir) {
  let pkg;
  try {
    pkg = JSON.parse(readFileSync(`${dir}/package.json`, 'utf8'));
  } catch {
    return false;
  }

  const dependencies = Object.keys(pkg.dependencies ?? {});
  return dependencies.some(name => !existsSync(`${dir}/node_modules/${name}`));
}

async function installDependencies(dir, timeoutMs) {
  let child;
  try {
    child = spawn(ANT, ['install'], { cwd: dir });
  } catch (error) {
    return { ok: false, detail: `could not start dependency install in ${dir}`, output: error.message };
  }
  let output = '';
  child.stdout.on('data', d => {
    output += d;
  });
  child.stderr.on('data', d => {
    output += d;
  });

  let timer;
  const timeout = new Promise(resolve => {
    timer = setTimeout(() => resolve('timeout'), timeoutMs);
  });
  const exited = new Promise(resolve => {
    child.on('exit', code => resolve(code));
    child.on('error', error => {
      output += `${error.message}\n`;
      resolve(-1);
    });
  });
  const result = await Promise.race([exited, timeout]);
  clearTimeout(timer);

  if (result === 'timeout') {
    try {
      child.kill();
    } catch {}
    return { ok: false, detail: `dependency install timed out in ${dir}`, output };
  }
  if (result !== 0) return { ok: false, detail: `dependency install failed in ${dir} (code ${result})`, output };
  return { ok: true };
}

async function ensureDependencies(target, opts) {
  const dir = npmExampleDir(target.entry);
  if (!dir || !needsInstall(dir)) return null;
  console.log(`INSTALL ${dir} (ant install)`);
  return installDependencies(dir, opts.targetTimeoutMs);
}

class Child {
  constructor(entry, args = []) {
    this.output = '';
    this.exitCode = null;
    this.proc = spawn(ANT, [entry, ...args]);
    this.proc.stdout.on('data', d => {
      this.output += d;
    });
    this.proc.stderr.on('data', d => {
      this.output += d;
    });
    this.exited = new Promise(resolve => {
      this.proc.on('exit', code => {
        this.exitCode = code;
        resolve(code);
      });
    });
  }

  kill() {
    try {
      this.proc.kill();
    } catch {}
  }

  async waitExit(timeoutMs) {
    let timer;
    const timeout = new Promise(resolve => {
      timer = setTimeout(() => resolve('timeout'), timeoutMs);
    });
    const result = await Promise.race([this.exited, timeout]);
    clearTimeout(timer);
    return result === 'timeout' ? 'timeout' : this.exitCode;
  }
}

async function portIsFree(url) {
  try {
    await fetchT(url, 500);
    return false;
  } catch {
    return true;
  }
}

async function waitReady(child, url, timeoutMs) {
  const deadline = now() + timeoutMs;
  while (now() < deadline) {
    if (child.exitCode !== null) return `server exited early (code ${child.exitCode})`;
    try {
      const res = await fetchT(url, 1000);
      if (res.ok || res.status < 500) return null;
    } catch {}
    await new Promise(r => setTimeout(r, 100));
  }
  return `not ready within ${timeoutMs}ms`;
}

async function driveLoad(url, total, concurrency) {
  let sent = 0;
  let failures = 0;
  let firstFailure = null;
  const ABORT_AFTER = 25;

  async function worker() {
    for (;;) {
      const i = sent++;
      if (i >= total || failures >= ABORT_AFTER) return;
      try {
        const res = await fetchT(url, 5000);
        if (!res.ok) {
          failures++;
          if (!firstFailure) firstFailure = `HTTP ${res.status} at request ${i}`;
        } else {
          await res.text();
        }
      } catch (e) {
        failures++;
        if (!firstFailure) firstFailure = `${e && e.message ? e.message : e} at request ${i}`;
      }
    }
  }

  await Promise.all(Array.from({ length: concurrency }, worker));
  return { failures, firstFailure };
}

const SNAPSHOT_DIR = 'tests/harness/snapshots';

const DEFAULT_SCRUB = [
  [/[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}/g, '<uuid>'],
  [/(^|[^A-Za-z0-9_-])[A-Za-z0-9_-]{21}(?=$|[^A-Za-z0-9_-])/gm, '$1<nanoid>'],
  [/t=\d{10,}/g, 't=<timestamp>']
];

function scrub(output, extra = []) {
  let out = output;
  for (const [re, repl] of [...DEFAULT_SCRUB, ...extra]) out = out.replace(re, repl);
  return out.replace(/\n+$/, '\n');
}

export async function runSnapshot(target, opts) {
  const child = new Child(target.entry);
  const code = await child.waitExit(opts.targetTimeoutMs);
  if (code === 'timeout') {
    child.kill();
    return { ok: false, detail: 'timed out' };
  }
  if (code !== 0) return { ok: false, detail: `exit ${code}`, output: child.output };

  const rc = runRangeChecks(child.output, target.checks ?? []);
  if (rc.failures.length) return { ok: false, detail: 'range check failed', output: rc.failures.join('\n') };

  const actual = scrub(child.output, target.scrub);
  const file = `${SNAPSHOT_DIR}/${target.name.replace(/[^A-Za-z0-9_-]/g, '_')}.txt`;

  let expected = null;
  try {
    expected = scrub(readFileSync(file, 'utf8'), target.scrub);
  } catch {}

  if (opts.update || expected === null) {
    mkdirSync(SNAPSHOT_DIR, { recursive: true });
    writeFileSync(file, actual);
    return { ok: true, detail: expected === null ? 'snapshot created' : 'snapshot updated' };
  }

  if (actual !== expected) {
    const a = actual.split('\n'),
      e = expected.split('\n');
    let i = 0;
    while (i < a.length && i < e.length && a[i] === e[i]) i++;
    return {
      ok: false,
      detail: `output diverges from snapshot at line ${i + 1} (rerun with --update if intended)`,
      output: `expected: ${e[i] ?? '<eof>'}\n  actual: ${a[i] ?? '<eof>'}`
    };
  }
  return { ok: true };
}

function runRangeChecks(output, checks) {
  const failures = [];
  const report = [];
  for (const c of checks) {
    const flags = c.re.flags.includes('g') ? c.re.flags : c.re.flags + 'g';
    const re = new RegExp(c.re.source, flags);
    let m,
      seen = 0;
    const values = [];
    while ((m = re.exec(output))) {
      seen++;
      values.push(m[1]);
      if (c.expected !== undefined) {
        if (String(m[1]) !== String(c.expected)) failures.push(`${c.name}: ${m[1]} != ${c.expected}`);
      } else if (c.nowToleranceMs !== undefined) {
        const v = Number(m[1]);
        if (!Number.isFinite(v) || Math.abs(v - Date.now()) > c.nowToleranceMs) {
          failures.push(`${c.name}: ${m[1]} is not within ${c.nowToleranceMs}ms of current time`);
        }
      } else {
        const v = parseFloat(String(m[1]).replace(/,/g, ''));
        if (!Number.isFinite(v) || v < c.min || v > c.max) failures.push(`${c.name}: ${m[1]} outside [${c.min}, ${c.max}]`);
      }
    }
    if (!seen) failures.push(`${c.name}: pattern never matched`);
    else if (c.count !== undefined && seen !== c.count) failures.push(`${c.name}: matched ${seen} times, expected ${c.count}`);
    if (seen) {
      const expectation =
        c.expected !== undefined
          ? `= ${c.expected}`
          : c.nowToleranceMs !== undefined
            ? `within ${c.nowToleranceMs}ms of current time`
            : `in [${c.min}, ${c.max}]`;
      report.push(`${c.name}: ${values.join(', ')} ${expectation}`);
    }
  }
  return { failures, report };
}

export async function runDemo(target, opts) {
  const child = new Child(target.entry);
  const code = await child.waitExit(opts.targetTimeoutMs);
  if (code === 'timeout') {
    child.kill();
    return { ok: false, detail: 'timed out' };
  }
  if (code !== 0) return { ok: false, detail: `exit ${code}`, output: child.output };
  const errs = scanForErrors(child.output);
  if (errs.length) return { ok: false, detail: 'error markers in output', output: errs.join('\n') };
  const { failures, report } = runRangeChecks(child.output, target.checks ?? []);
  if (failures.length) return { ok: false, detail: 'range check failed', output: failures.join('\n') };
  return { ok: true, report };
}

function scanForErrors(output) {
  const lines = output.split('\n').filter(l => ERROR_MARKERS.test(l));
  return lines.slice(0, 3);
}

export async function runSpec(target, opts) {
  const child = new Child(target.entry);
  const code = await child.waitExit(opts.targetTimeoutMs);
  if (code === 'timeout') {
    child.kill();
    return { ok: false, detail: 'timed out' };
  }
  const failed = (child.output.match(/Failed:\s*(\d+)/) || [])[1];
  if (code !== 0) return { ok: false, detail: `exit ${code}`, output: child.output };
  if (failed !== undefined && Number(failed) > 0) return { ok: false, detail: `${failed} assertions failed`, output: child.output };
  return { ok: true };
}

export async function runTest(target, opts) {
  const child = new Child(target.entry);
  const code = await child.waitExit(opts.targetTimeoutMs);
  if (code === 'timeout') {
    child.kill();
    return { ok: false, detail: 'timed out' };
  }
  const errs = scanForErrors(child.output);
  if (code !== 0) return { ok: false, detail: `exit ${code}`, output: child.output };
  if (errs.length) return { ok: false, detail: 'error markers in output', output: errs.join('\n') };
  return { ok: true };
}

export const runScript = runTest;

export async function runServer(target, opts) {
  const port = target.port ?? 3000;
  const url = `http://127.0.0.1:${port}${target.path ?? '/'}`;

  if (!(await portIsFree(url))) return { ok: false, detail: `port ${port} already in use before spawn` };

  const child = new Child(target.entry);
  try {
    const notReady = await waitReady(child, url, opts.readyTimeoutMs);
    if (notReady) return { ok: false, detail: notReady, output: child.output };

    const total = target.requests ?? opts.requests;
    const driveStart = now();
    const { failures, firstFailure } = await driveLoad(url, total, target.concurrency ?? opts.concurrency);
    const driveMs = now() - driveStart;
    const errs = scanForErrors(child.output);
    if (failures > 0) return { ok: false, detail: `${failures} failed requests (${firstFailure})`, output: errs.join('\n') };
    if (errs.length) return { ok: false, detail: 'runtime errors in server output', output: errs.join('\n') };
    const rps = Math.round(total / (driveMs / 1000));
    return { ok: true, detail: `${total} reqs, ${rps} rps` };
  } finally {
    child.kill();
    await child.waitExit(3000);
  }
}

let ohaPath = undefined;
async function ohaAvailable() {
  if (ohaPath !== undefined) return ohaPath;
  try {
    const probe = spawn('oha', ['--version']);
    ohaPath = await new Promise(resolve => {
      probe.on('exit', code => resolve(code === 0));
      probe.on('error', () => resolve(false));
      setTimeout(() => resolve(false), 2000);
    });
  } catch {
    ohaPath = false;
  }
  return ohaPath;
}

export async function runOha(target, opts) {
  if (!(await ohaAvailable())) return { ok: true, detail: 'skipped: oha not installed' };

  const port = target.port ?? 3000;
  const url = `http://127.0.0.1:${port}${target.path ?? '/'}`;
  if (!(await portIsFree(url))) return { ok: false, detail: `port ${port} already in use before spawn` };

  const child = new Child(target.entry);
  try {
    const notReady = await waitReady(child, url, opts.readyTimeoutMs);
    if (notReady) return { ok: false, detail: notReady, output: child.output };

    let ohaOut = '';
    const oha = spawn('oha', [url, '-n', '50000', '-H', 'Accept-Encoding: identity', '--no-tui']);
    oha.on('stdout', d => {
      ohaOut += d;
    });
    oha.on('stderr', d => {
      ohaOut += d;
    });
    const done = await new Promise(resolve => {
      oha.on('exit', code => resolve(code));
      setTimeout(() => {
        try {
          oha.kill();
        } catch {}
        resolve('timeout');
      }, opts.targetTimeoutMs);
    });
    if (done === 'timeout') return { ok: false, detail: 'oha timed out' };

    const rps = parseFloat((ohaOut.match(/Requests\/sec:\s+([\d.]+)/) || [])[1]);
    const success = parseFloat((ohaOut.match(/Success rate:\s+([\d.]+)/) || [])[1]);
    const errs = scanForErrors(child.output);

    if (!Number.isFinite(rps)) return { ok: false, detail: 'could not parse oha output', output: ohaOut.slice(0, 200) };
    if (success !== 100) return { ok: false, detail: `success rate ${success}%`, output: errs.join('\n') };
    if (errs.length) return { ok: false, detail: 'runtime errors in server output', output: errs.join('\n') };
    const summary = `${Math.round(rps)} rps (ref ~${target.refRps}, min ${target.minRps})`;
    if (rps < target.minRps) return { ok: false, detail: `throughput below floor: ${summary}` };
    return { ok: true, detail: summary };
  } finally {
    child.kill();
    await child.waitExit(3000);
  }
}

const RUNNERS = { spec: runSpec, test: runTest, script: runScript, server: runServer, snapshot: runSnapshot, demo: runDemo, oha: runOha };

export async function run(targets, opts = {}) {
  const o = { ...defaults, ...opts };
  let pass = 0,
    fail = 0;
  const failures = [];

  let group = null;
  for (const t of targets) {
    if (t.group !== group) {
      group = t.group;
      console.log(`\n== ${group} ==`);
    }
    if (t.type === 'skip') continue;
    const started = now();
    const installResult = await ensureDependencies(t, o);
    const result = installResult && !installResult.ok ? installResult : await RUNNERS[t.type](t, o);
    const ms = (now() - started).toFixed(0);
    if (result.ok) {
      pass++;
      console.log(`PASS ${t.name} (${ms}ms)${result.detail ? ' — ' + result.detail : ''}`);
      if (result.report) for (const line of result.report) console.log(`     ${line}`);
    } else {
      fail++;
      failures.push({ name: t.name, ...result });
      console.log(`FAIL ${t.name} (${ms}ms) — ${result.detail}`);
      if (result.output) {
        for (const line of String(result.output).split('\n').slice(0, 4)) if (line.trim()) console.log(`     ${line}`);
      }
    }
  }

  console.log(`\nharness: ${pass} passed, ${fail} failed`);
  return fail === 0;
}
