#!/usr/bin/env node

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  benchmarkFiles,
  buildMetadata,
  createBenchmarkSources,
  executableMetadata,
  geometricMean,
  hostMetadata,
  repositoryMetadata,
  resolveEngine,
  rotate,
  runBenchmarkProcess,
  runExecutionId,
  runKey,
  scheduledDate,
  suiteRevision,
  summarizeEngineSamples
} from './lib.mjs';

const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const defaultRepo = path.resolve(scriptDir, '..', '..');

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const repo = path.resolve(options.repo || defaultRepo);
  const engineRoot = path.resolve(options.engineRoot || path.join(repo, '.cache', 'arm64-bench'));
  const configPath = path.resolve(options.config || path.join(scriptDir, 'engines.json'));
  const config = JSON.parse(fs.readFileSync(configPath, 'utf8'));
  const statePath = path.join(engineRoot, 'engine-state.json');
  const engineState = fs.existsSync(statePath)
    ? JSON.parse(fs.readFileSync(statePath, 'utf8')).engines || {}
    : {};
  const selected = new Set(options.engines);
  const engines = config.engines
    .filter(engine => selected.size === 0 || selected.has(engine.id))
    .map(engine => resolveEngine(engine, { repo, engineRoot }));

  if (engines.length === 0) throw new Error('no engines selected');
  if (options.samples < 1 || options.samples > 25) throw new Error('--samples must be between 1 and 25');

  const availableEngines = engines.filter(engine => fs.existsSync(engine.executable));

  const startedAt = new Date();
  const date = options.date || scheduledDate(startedAt);
  const repository = await repositoryMetadata(repo);
  const suiteHash = suiteRevision(repo);
  const resultEngines = new Map();
  const temporaryDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-arm64-bench-'));

  try {
    const sources = createBenchmarkSources(repo, temporaryDir);
    for (const engine of engines) {
      const available = fs.existsSync(engine.executable);
      const identity = available
        ? await executableMetadata(engine)
        : {
            version: 'unavailable',
            versionError: `missing executable: ${engine.executable}`,
            executableSha256: '0'.repeat(64)
          };
      resultEngines.set(engine.id, {
        id: engine.id,
        name: engine.name,
        mode: engine.mode,
        status: available ? 'pending' : 'unavailable',
        version: identity.version === 'unknown' && engineState[engine.id]?.resolvedVersion
          ? engineState[engine.id].resolvedVersion
          : identity.version,
        versionError: identity.versionError,
        channel: engine.id === 'ant' ? 'repository' : engineState[engine.id]?.channel || 'unresolved',
        resolvedVersion: engine.id === 'ant'
          ? identity.version
          : engineState[engine.id]?.resolvedVersion || identity.version,
        sourceRevision: engine.id === 'ant'
          ? repository.revision
          : engineState[engine.id]?.sourceRevision || null,
        retrievedAt: engine.id === 'ant' ? null : engineState[engine.id]?.retrievedAt || null,
        executableSha256: identity.executableSha256,
        flags: engine.args,
        samples: []
      });
    }

    for (let sampleIndex = 0; sampleIndex < options.samples; sampleIndex++) {
      const engineOrder = rotate(availableEngines, sampleIndex);
      for (const engine of engineOrder) {
        const target = resultEngines.get(engine.id);
        const sampleStarted = Date.now();
        const sample = {
          index: sampleIndex + 1,
          status: 'ok',
          score: null,
          durationMs: 0,
          results: {},
          rawScores: {},
          errors: []
        };

        process.stdout.write(`sample ${sampleIndex + 1}/${options.samples}: ${engine.id}\n`);
        const testOrder = rotate(benchmarkFiles, sampleIndex + engines.indexOf(engine));
        for (const [name] of testOrder) {
          process.stdout.write(`  ${name}\n`);
          const outcome = await runBenchmarkProcess(engine, sources.get(name), name, options.timeoutMs);
          if (outcome.status === 'ok') {
            sample.results[name] = outcome.score;
            sample.rawScores[name] = outcome.rawScore;
          } else {
            sample.status = 'failed';
            sample.errors.push({ benchmark: name, status: outcome.status, message: outcome.error });
          }
        }

        sample.durationMs = Date.now() - sampleStarted;
        if (sample.status === 'ok') {
          sample.score = geometricMean(benchmarkFiles.map(([name]) => sample.rawScores[name]));
          if (!Number.isFinite(sample.score)) {
            sample.status = 'failed';
            sample.errors.push({ benchmark: 'aggregate', status: 'failed', message: 'invalid geometric mean' });
          }
        }
        target.samples.push(sample);
      }
    }
  } finally {
    fs.rmSync(temporaryDir, { recursive: true, force: true });
  }

  for (const engine of resultEngines.values()) {
    engine.summary = summarizeEngineSamples(engine.samples);
    if (engine.status !== 'unavailable') {
      engine.status = engine.summary.sampleCount === options.samples ? 'ok' : 'failed';
    }
  }

  const finishedAt = new Date();
  const enginesOutput = [...resultEngines.values()];
  const output = {
    schema: 1,
    run: {
      key: runKey(date, repository.revision, suiteHash, runExecutionId(startedAt)),
      scheduledDate: date,
      startedAt: startedAt.toISOString(),
      finishedAt: finishedAt.toISOString(),
      status: enginesOutput.every(engine => engine.status === 'ok') ? 'ok' : 'failed',
      samplesRequested: options.samples,
      durationMs: finishedAt.getTime() - startedAt.getTime()
    },
    repository,
    suite: {
      name: config.suite,
      protocol: config.protocol,
      revision: suiteHash,
      benchmarks: benchmarkFiles.map(([name]) => name)
    },
    build: await buildMetadata(repo, engineRoot),
    host: hostMetadata(),
    engines: enginesOutput
  };

  const serialized = `${JSON.stringify(output, null, 2)}\n`;
  if (options.output === '-') process.stdout.write(serialized);
  else {
    fs.mkdirSync(path.dirname(path.resolve(options.output)), { recursive: true });
    fs.writeFileSync(options.output, serialized);
    process.stdout.write(`wrote ${options.output}\n`);
  }

  if (options.strict && output.run.status !== 'ok') process.exitCode = 1;
}

function parseArgs(argv) {
  const options = {
    config: null,
    date: null,
    engineRoot: null,
    engines: [],
    output: 'arm64-bench-result.json',
    repo: null,
    samples: 5,
    strict: false,
    timeoutMs: 10 * 60 * 1000
  };
  for (let index = 0; index < argv.length; index++) {
    const arg = argv[index];
    if (arg === '--strict') options.strict = true;
    else if (arg === '--engine') options.engines.push(requiredValue(argv, ++index, arg));
    else if (arg === '--samples') options.samples = Number(requiredValue(argv, ++index, arg));
    else if (arg === '--timeout-ms') options.timeoutMs = Number(requiredValue(argv, ++index, arg));
    else if (arg === '--output') options.output = requiredValue(argv, ++index, arg);
    else if (arg === '--config') options.config = requiredValue(argv, ++index, arg);
    else if (arg === '--engine-root') options.engineRoot = requiredValue(argv, ++index, arg);
    else if (arg === '--repo') options.repo = requiredValue(argv, ++index, arg);
    else if (arg === '--date') options.date = requiredValue(argv, ++index, arg);
    else if (arg === '--help') {
      process.stdout.write('Usage: node tools/arm64-bench/run.mjs [--samples N] [--engine ID] [--output PATH] [--strict]\n');
      process.exit(0);
    } else throw new Error(`unknown argument: ${arg}`);
  }
  if (!Number.isInteger(options.samples)) throw new Error('--samples must be an integer');
  if (!Number.isFinite(options.timeoutMs) || options.timeoutMs < 1000) {
    throw new Error('--timeout-ms must be at least 1000');
  }
  if (options.date && !/^\d{4}-\d{2}-\d{2}$/u.test(options.date)) {
    throw new Error('--date must use YYYY-MM-DD');
  }
  return options;
}

function requiredValue(argv, index, option) {
  if (!argv[index]) throw new Error(`${option} requires a value`);
  return argv[index];
}

main().catch(error => {
  console.error(error instanceof Error ? error.stack : error);
  process.exitCode = 1;
});
