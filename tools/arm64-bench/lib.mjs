import childProcess from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { promisify } from 'node:util';

const execFile = promisify(childProcess.execFile);

export const benchmarkFiles = [
  ['Richards', 'tests/richards.js'],
  ['DeltaBlue', 'tests/deltablue.js'],
  ['Crypto', 'tests/crypto.js'],
  ['RayTrace', 'tests/raytrace.js'],
  ['EarleyBoyer', 'tests/earley-boyer.js'],
  ['RegExp', 'tests/regexp.js'],
  ['Splay', 'tests/splay.js'],
  ['NavierStokes', 'tests/navier-stokes.js']
];

export function median(values) {
  if (values.length === 0) return null;
  const sorted = [...values].sort((left, right) => left - right);
  const middle = Math.floor(sorted.length / 2);
  return sorted.length % 2 === 0 ? (sorted[middle - 1] + sorted[middle]) / 2 : sorted[middle];
}

export function summarize(values) {
  if (values.length === 0) return null;
  const center = median(values);
  return {
    median: center,
    mad: median(values.map(value => Math.abs(value - center))),
    min: Math.min(...values),
    max: Math.max(...values)
  };
}

export function geometricMean(values) {
  if (values.length === 0 || values.some(value => !Number.isFinite(value) || value <= 0)) {
    return null;
  }
  return Math.exp(values.reduce((sum, value) => sum + Math.log(value), 0) / values.length);
}

export function rotate(values, offset) {
  if (values.length === 0) return [];
  const normalized = ((offset % values.length) + values.length) % values.length;
  return [...values.slice(normalized), ...values.slice(0, normalized)];
}

export function parseBenchmarkOutput(stdout, expectedName) {
  const resultMatch = stdout.match(/^result\s+(.+?)\s+(\S+)\s*$/im);
  const rawScoreMatch = stdout.match(/^raw-score\s+(\S+)\s*$/im);
  if (!resultMatch || !rawScoreMatch) throw new Error('missing benchmark result lines');

  const name = resultMatch[1];
  const score = Number(resultMatch[2]);
  const rawScore = Number(rawScoreMatch[1]);
  if (name !== expectedName) throw new Error(`expected ${expectedName}, received ${name}`);
  if (!Number.isFinite(score) || score <= 0) throw new Error(`invalid score ${resultMatch[2]}`);
  if (!Number.isFinite(rawScore) || rawScore <= 0) {
    throw new Error(`invalid raw score ${rawScoreMatch[1]}`);
  }
  return { name, score, rawScore };
}

export function interpolatePath(value, context) {
  let output = value.replaceAll('{repo}', context.repo).replaceAll('{engineRoot}', context.engineRoot);
  output = output.replace(/\{brew:([a-z0-9@+_.-]+)\}/gu, (_match, formula) => {
    const prefix = childProcess.execFileSync('brew', ['--prefix', formula], {
      encoding: 'utf8'
    });
    return prefix.trim();
  });
  return output;
}

export function resolveEngine(engine, context) {
  const overrideName = `ANT_BENCH_ENGINE_${engine.id.toUpperCase().replaceAll('-', '_')}`;
  const configured = process.env[overrideName] || engine.executable;
  return {
    ...engine,
    executable: interpolatePath(configured, context),
    args: engine.args || [],
    versionArgs: engine.versionArgs || []
  };
}

export async function executableMetadata(engine, timeoutMs = 30_000) {
  const bytes = fs.readFileSync(engine.executable);
  let version = 'unknown';
  let versionError = null;
  if (engine.versionArgs.length > 0) {
    try {
      const result = await execFile(engine.executable, engine.versionArgs, {
        encoding: 'utf8',
        timeout: timeoutMs,
        maxBuffer: 1024 * 1024
      });
      version = `${result.stdout}\n${result.stderr}`.trim().split(/\r?\n/u)[0] || version;
    } catch (error) {
      versionError = compactError(error);
    }
  }
  return {
    version,
    versionError,
    executableSha256: crypto.createHash('sha256').update(bytes).digest('hex')
  };
}

export function createBenchmarkSources(repo, temporaryDir) {
  const benchmarkDir = path.join(repo, 'examples', 'bench-v8');
  const base = fs.readFileSync(path.join(benchmarkDir, 'tests', 'base.js'), 'utf8');
  const harness = fs.readFileSync(path.join(repo, 'tools', 'arm64-bench', 'portable-harness.js'), 'utf8');
  const sources = new Map();

  for (const [name, relativePath] of benchmarkFiles) {
    const source = `${base}\n${fs.readFileSync(path.join(benchmarkDir, relativePath), 'utf8')}\n${harness}\n`;
    const sourcePath = path.join(temporaryDir, `${name}.js`);
    fs.writeFileSync(sourcePath, source);
    sources.set(name, sourcePath);
  }
  return sources;
}

export function suiteRevision(repo) {
  const hash = crypto.createHash('sha256');
  const relativeFiles = [
    'examples/bench-v8/tests/base.js',
    ...benchmarkFiles.map(([_name, file]) => `examples/bench-v8/${file}`),
    'tools/arm64-bench/portable-harness.js'
  ];
  for (const file of relativeFiles) {
    hash.update(file);
    hash.update('\0');
    hash.update(fs.readFileSync(path.join(repo, file)));
    hash.update('\0');
  }
  return hash.digest('hex');
}

export async function runBenchmarkProcess(engine, sourcePath, expectedName, timeoutMs) {
  const started = process.hrtime.bigint();
  try {
    const result = await execFile(engine.executable, [...engine.args, sourcePath], {
      encoding: 'utf8',
      timeout: timeoutMs,
      maxBuffer: 16 * 1024 * 1024,
      env: { ...process.env, NO_COLOR: '1' }
    });
    let parsed;
    try {
      parsed = parseBenchmarkOutput(result.stdout, expectedName);
    } catch (error) {
      error.stdout = result.stdout;
      error.stderr = result.stderr;
      throw error;
    }
    return {
      status: 'ok',
      score: parsed.score,
      rawScore: parsed.rawScore,
      durationMs: elapsedMs(started)
    };
  } catch (error) {
    return {
      status: error.killed || error.signal === 'SIGTERM' ? 'timeout' : 'failed',
      durationMs: elapsedMs(started),
      error: compactError(error)
    };
  }
}

export function summarizeEngineSamples(samples) {
  const successful = samples.filter(sample => sample.status === 'ok');
  const results = {};
  for (const [name] of benchmarkFiles) {
    results[name] = summarize(
      successful.map(sample => sample.results[name]).filter(value => Number.isFinite(value))
    );
  }
  return {
    sampleCount: successful.length,
    score: summarize(successful.map(sample => sample.score)),
    results
  };
}

export async function repositoryMetadata(repo) {
  const git = async args => (await execFile('git', args, { cwd: repo, encoding: 'utf8' })).stdout.trim();
  const revision = await git(['rev-parse', 'HEAD']);
  const branch = process.env.GITHUB_REF_NAME || (await git(['branch', '--show-current'])) || 'detached';
  const status = await git(['status', '--porcelain', '--untracked-files=no']);
  return {
    url: process.env.GITHUB_SERVER_URL && process.env.GITHUB_REPOSITORY
      ? `${process.env.GITHUB_SERVER_URL}/${process.env.GITHUB_REPOSITORY}`
      : 'https://github.com/theMackabu/ant',
    branch,
    revision,
    dirty: status.length > 0
  };
}

export function hostMetadata() {
  return {
    runner: 'arm64-bench',
    os: os.platform(),
    release: os.release(),
    arch: os.arch(),
    cpuCount: os.cpus().length,
    memoryBytes: os.totalmem()
  };
}

export async function buildMetadata(repo, engineRoot = path.join(repo, '.cache', 'arm64-bench')) {
  const manifestPath = path.join(engineRoot, 'ant-bench-build', 'arm64-bench-build.json');
  if (fs.existsSync(manifestPath)) {
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    if (
      typeof manifest.type === 'string'
      && typeof manifest.compiler === 'string'
      && typeof manifest.tuning === 'string'
      && (manifest.pgoProfileSha256 === null || /^[0-9a-f]{64}$/u.test(manifest.pgoProfileSha256))
    ) {
      return manifest;
    }
    throw new Error(`invalid benchmark build manifest: ${manifestPath}`);
  }

  const compiler = process.env.CC || 'clang';
  let compilerVersion = 'unknown';
  try {
    const result = await execFile(compiler, ['--version'], { encoding: 'utf8', timeout: 30_000 });
    compilerVersion = result.stdout.trim().split(/\r?\n/u)[0] || compilerVersion;
  } catch {
    // The binary identity and result remain useful if a compiler was not on PATH.
  }

  const profilePath = path.join(repo, 'meson', 'pgo', 'profiles', 'ant-darwin-aarch64.profdata');
  return {
    type: process.env.ANT_BENCH_BUILD_TYPE || 'nix-develop-release-pgo-lto',
    compiler: compilerVersion,
    tuning: process.env.ANT_BENCH_TUNING || 'native-arm64',
    pgoProfileSha256: fs.existsSync(profilePath)
      ? crypto.createHash('sha256').update(fs.readFileSync(profilePath)).digest('hex')
      : null
  };
}

export function scheduledDate(now = new Date()) {
  const formatter = new Intl.DateTimeFormat('en-CA', {
    timeZone: 'America/Los_Angeles',
    year: 'numeric',
    month: '2-digit',
    day: '2-digit'
  });
  const fields = Object.fromEntries(formatter.formatToParts(now).map(part => [part.type, part.value]));
  return `${fields.year}-${fields.month}-${fields.day}`;
}

export function runExecutionId(now = new Date(), environment = process.env) {
  if (/^\d+$/u.test(environment.GITHUB_RUN_ID || '')) {
    const attempt = /^\d+$/u.test(environment.GITHUB_RUN_ATTEMPT || '')
      ? environment.GITHUB_RUN_ATTEMPT
      : '1';
    return `gh-${environment.GITHUB_RUN_ID}-${attempt}`;
  }
  return `local-${now.toISOString().replace(/\D/gu, '')}`;
}

export function runKey(date, repositoryRevision, suiteHash, executionId) {
  return `arm64-bench:${date}:${repositoryRevision.slice(0, 12)}:${suiteHash.slice(0, 12)}:${executionId}`;
}

export function compactError(error) {
  const stderr = typeof error.stderr === 'string' ? error.stderr.trim() : '';
  const stdout = typeof error.stdout === 'string' ? error.stdout.trim() : '';
  const message = error instanceof Error ? error.message : String(error);
  return [message, stderr, stdout].filter(Boolean).join('\n').slice(0, 4096);
}

function elapsedMs(started) {
  return Number(process.hrtime.bigint() - started) / 1_000_000;
}
