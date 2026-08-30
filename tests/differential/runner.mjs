#!/usr/bin/env node

import childProcess from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { familyNames, generateCases } from './probes.mjs';

const toolDir = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(toolDir, '../..');

function usage() {
  return `Usage: node tests/differential/runner.mjs [options]

Compare generated probes under Node and Ant.

Options:
  --ant PATH          Ant executable (default: ./build/ant)
  --node PATH         Node executable (default: current Node)
  --family NAME       Probe family; repeatable (default: all)
  --cases N           Cases per family (default: 1)
  --seed N            Deterministic generator seed (default: 1)
  --timeout MS        Per-engine timeout (default: 5000)
  --minimize          Minimize each mismatch and save a standalone repro
  --output DIR        Repro directory (default: differential-output)
  --json              Emit newline-delimited JSON records
  --list-families     Print available families
  --help              Show this help
`;
}

export function parseArgs(argv) {
  const options = {
    ant: path.join(repoRoot, 'build/ant'),
    node: process.execPath,
    families: [],
    casesPerFamily: 1,
    seed: 1,
    timeout: 5000,
    minimize: false,
    output: path.join(repoRoot, 'differential-output'),
    json: false,
    listFamilies: false,
    help: false
  };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    const take = name => {
      const value = argv[++index];
      if (value === undefined) throw new Error(`${name} requires a value`);
      return value;
    };
    if (argument === '--ant') options.ant = path.resolve(take(argument));
    else if (argument === '--node') options.node = path.resolve(take(argument));
    else if (argument === '--family') options.families.push(take(argument));
    else if (argument === '--cases') options.casesPerFamily = Number(take(argument));
    else if (argument === '--seed') options.seed = Number(take(argument));
    else if (argument === '--timeout') options.timeout = Number(take(argument));
    else if (argument === '--output') options.output = path.resolve(take(argument));
    else if (argument === '--minimize') options.minimize = true;
    else if (argument === '--json') options.json = true;
    else if (argument === '--list-families') options.listFamilies = true;
    else if (argument === '--help' || argument === '-h') options.help = true;
    else throw new Error(`unknown option: ${argument}`);
  }
  if (options.families.length === 0) options.families = [...familyNames];
  for (const family of options.families) if (!familyNames.includes(family)) throw new Error(`unknown family: ${family}`);
  for (const [name, value] of [
    ['cases', options.casesPerFamily],
    ['seed', options.seed],
    ['timeout', options.timeout]
  ]) {
    if (!Number.isSafeInteger(value) || value < (name === 'seed' ? 0 : 1)) throw new Error(`--${name} must be a valid integer`);
  }
  return options;
}

const normalizeNewlines = value => String(value || '').replace(/\r\n/g, '\n');

export function runEngine(executable, sourceFile, timeout) {
  const result = childProcess.spawnSync(executable, [sourceFile], {
    cwd: repoRoot,
    encoding: 'utf8',
    timeout,
    maxBuffer: 4 * 1024 * 1024,
    env: { ...process.env, NO_COLOR: '1', FORCE_COLOR: '0' }
  });
  return {
    status: result.status,
    signal: result.signal,
    timedOut: result.error?.code === 'ETIMEDOUT',
    stdout: normalizeNewlines(result.stdout),
    stderr: normalizeNewlines(result.stderr),
    spawnError: result.error && result.error.code !== 'ETIMEDOUT' ? `${result.error.code}: ${result.error.message}` : null
  };
}

export function sameResult(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

function compareCase(testCase, params, options, tempDir) {
  const source = testCase.build(params);
  const sourceFile = path.join(tempDir, `${testCase.id}.cjs`);
  fs.writeFileSync(sourceFile, source);
  const node = runEngine(options.node, sourceFile, options.timeout);
  const ant = runEngine(options.ant, sourceFile, options.timeout);
  return { mismatch: !sameResult(node, ant), source, node, ant };
}

export function minimizeArrays(testCase, initialParams, mismatchFor) {
  let params = structuredClone(initialParams);
  for (const key of testCase.shrinkKeys || []) {
    let values = params[key];
    if (!Array.isArray(values) || values.length < 2) continue;
    let granularity = 2;
    while (values.length >= 2) {
      const chunkSize = Math.ceil(values.length / granularity);
      let reduced = false;
      for (let start = 0; start < values.length; start += chunkSize) {
        const candidateValues = values.slice(0, start).concat(values.slice(start + chunkSize));
        if (candidateValues.length === 0) continue;
        const candidate = { ...params, [key]: candidateValues };
        if (mismatchFor(candidate)) {
          params = candidate;
          values = candidateValues;
          granularity = Math.max(2, granularity - 1);
          reduced = true;
          break;
        }
      }
      if (reduced) continue;
      if (granularity >= values.length) break;
      granularity = Math.min(values.length, granularity * 2);
    }
  }
  return params;
}

const compactResult = result => ({
  status: result.status,
  signal: result.signal,
  timedOut: result.timedOut,
  stdout: result.stdout,
  stderr: result.stderr,
  spawnError: result.spawnError
});

function printRecord(record, json) {
  if (json) return console.log(JSON.stringify(record));
  console.log(`${record.match ? 'PASS' : 'MISMATCH'} ${record.family}/${record.id}`);
  if (!record.match) {
    console.log(`  node: ${JSON.stringify(record.node)}`);
    console.log(`  ant:  ${JSON.stringify(record.ant)}`);
    if (record.repro) console.log(`  repro: ${path.relative(repoRoot, record.repro)}`);
  }
}

export function main(argv = process.argv.slice(2)) {
  let options;
  try {
    options = parseArgs(argv);
  } catch (error) {
    console.error(error.message);
    console.error(usage());
    return 2;
  }
  if (options.help) {
    console.log(usage());
    return 0;
  }
  if (options.listFamilies) {
    console.log(familyNames.join('\n'));
    return 0;
  }
  for (const [label, executable] of [
    ['Node', options.node],
    ['Ant', options.ant]
  ]) {
    if (!fs.existsSync(executable)) {
      console.error(`${label} executable not found: ${executable}`);
      return 2;
    }
  }

  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-differential-'));
  const cases = generateCases(options);
  let mismatches = 0;
  try {
    for (const testCase of cases) {
      let comparison = compareCase(testCase, testCase.params, options, tempDir);
      let repro = null;
      if (comparison.mismatch) {
        mismatches += 1;
        if (options.minimize) {
          const minimized = minimizeArrays(testCase, testCase.params, candidate => compareCase(testCase, candidate, options, tempDir).mismatch);
          comparison = compareCase(testCase, minimized, options, tempDir);
          fs.mkdirSync(options.output, { recursive: true });
          repro = path.join(options.output, `${testCase.id}.cjs`);
          fs.writeFileSync(repro, comparison.source);
        }
      }
      printRecord(
        {
          family: testCase.family,
          id: testCase.id,
          seed: options.seed,
          match: !comparison.mismatch,
          node: compactResult(comparison.node),
          ant: compactResult(comparison.ant),
          repro
        },
        options.json
      );
    }
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
  if (!options.json) console.log(`\n${cases.length} probes, ${mismatches} mismatches`);
  return mismatches === 0 ? 0 : 1;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) process.exitCode = main();
