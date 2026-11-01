#!/usr/bin/env node
// Generates probes with fast-check, runs each under Node and Ant (and any
// Ant variants a family lists), and reports the first mismatch per family,
// shrunk to a minimal case with --minimize.

import fc from 'fast-check';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';
import { defaultVariants, differingLabels, repoRoot, runEngine, sameVariantResult, type EngineResult } from './engine.ts';
import { familyNamed, familyNames } from './families/index.ts';
import type { AnyFamily } from './families/family.ts';

export interface Options {
  ant: string;
  node: string;
  families: string[];
  casesPerFamily: number;
  seed: number;
  timeout: number;
  minimize: boolean;
  output: string;
  json: boolean;
  listFamilies: boolean;
  help: boolean;
}

function usage(): string {
  return `Usage: node tests/differential/runner.ts [options]

Compare generated probes under Node and Ant.

Options:
  --ant PATH          Ant executable (default: ./build/ant)
  --node PATH         Node executable (default: current Node)
  --family NAME       Probe family; repeatable (default: all)
  --cases N           Generated cases per family (default: 1)
  --seed N            Deterministic generator seed (default: 1)
  --timeout MS        Per-engine timeout (default: 5000)
  --minimize          Shrink each mismatch and save a standalone repro
  --output DIR        Repro directory (default: differential-output)
  --json              Emit one JSON record per family
  --list-families     Print available families
  --help              Show this help
`;
}

export function parseArgs(argv: readonly string[]): Options {
  const options: Options = {
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
    const take = (): string => {
      const value = argv[++index];
      if (value === undefined) throw new Error(`${argument} requires a value`);
      return value;
    };
    if (argument === '--ant') options.ant = path.resolve(take());
    else if (argument === '--node') options.node = path.resolve(take());
    else if (argument === '--family') options.families.push(take());
    else if (argument === '--cases') options.casesPerFamily = Number(take());
    else if (argument === '--seed') options.seed = Number(take());
    else if (argument === '--timeout') options.timeout = Number(take());
    else if (argument === '--output') options.output = path.resolve(take());
    else if (argument === '--minimize') options.minimize = true;
    else if (argument === '--json') options.json = true;
    else if (argument === '--list-families') options.listFamilies = true;
    else if (argument === '--help' || argument === '-h') options.help = true;
    else throw new Error(`unknown option: ${argument}`);
  }
  if (options.families.length === 0) options.families = [...familyNames];
  for (const family of options.families) if (!familyNames.includes(family)) throw new Error(`unknown family: ${family}`);
  for (const [name, value] of [['cases', options.casesPerFamily], ['seed', options.seed], ['timeout', options.timeout]] as const) {
    if (!Number.isSafeInteger(value) || value < (name === 'seed' ? 0 : 1)) throw new Error(`--${name} must be a valid integer`);
  }
  return options;
}

interface Comparison {
  mismatch: boolean;
  source: string;
  node: EngineResult;
  ant: EngineResult;
  failing: string[];
}

let probeCounter = 0;

function compare(family: AnyFamily, params: unknown, options: Options, tempDir: string): Comparison {
  const source = family.build(params);
  const sourceFile = path.join(tempDir, `${family.name}-${probeCounter++}.cjs`);
  fs.writeFileSync(sourceFile, source);
  const node = runEngine(options.node, sourceFile, options.timeout);
  const variants = family.variants || defaultVariants;
  const results = variants.map(variant => runEngine(options.ant, sourceFile, options.timeout, variant));
  fs.rmSync(sourceFile, { force: true });
  const failing = variants.filter((variant, i) => !sameVariantResult(node, results[i], variant)).map(variant => variant.name);
  const shown = variants.findIndex(variant => failing.includes(variant.name));
  return { mismatch: failing.length > 0, source, node, ant: results[shown < 0 ? 0 : shown], failing };
}

const compactResult = (result: EngineResult) => ({
  status: result.status,
  signal: result.signal,
  timedOut: result.timedOut,
  stdout: result.stdout,
  stderr: result.stderr,
  spawnError: result.spawnError
});

interface FamilyRecord {
  family: string;
  seed: number;
  runs: number;
  shrinks: number;
  match: boolean;
  variants: string[];
  labels: string[];
  node?: ReturnType<typeof compactResult>;
  ant?: ReturnType<typeof compactResult>;
  error?: string;
  repro: string | null;
}

function runFamily(family: AnyFamily, options: Options, tempDir: string): FamilyRecord {
  // without --minimize, stop at the first mismatch instead of shrinking it
  const details = fc.check(
    fc.property(family.arbitrary, params => !compare(family, params, options, tempDir).mismatch),
    { seed: options.seed, numRuns: options.casesPerFamily, endOnFailure: !options.minimize }
  );
  const record: FamilyRecord = {
    family: family.name,
    seed: options.seed,
    runs: details.numRuns,
    shrinks: details.numShrinks,
    match: !details.failed,
    variants: [],
    labels: [],
    repro: null
  };
  if (!details.failed) return record;
  if (!details.counterexample) {
    record.error = String(details.errorInstance);
    return record;
  }

  const comparison = compare(family, details.counterexample[0], options, tempDir);
  record.variants = comparison.failing;
  record.labels = differingLabels(comparison.node, comparison.ant);
  record.node = compactResult(comparison.node);
  record.ant = compactResult(comparison.ant);
  if (options.minimize) {
    fs.mkdirSync(options.output, { recursive: true });
    record.repro = path.join(options.output, `${family.name}-seed${options.seed}.cjs`);
    fs.writeFileSync(record.repro, comparison.source);
  }
  return record;
}

function printRecord(record: FamilyRecord, json: boolean): void {
  if (json) return void console.log(JSON.stringify(record));
  if (record.match) return void console.log(`PASS ${record.family} (${record.runs} cases)`);
  console.log(`MISMATCH ${record.family} after ${record.runs} cases${record.shrinks ? `, ${record.shrinks} shrinks` : ''}`);
  if (record.error) return void console.log(`  error: ${record.error}`);
  console.log(`  ant variants: ${record.variants.join(', ')}`);
  if (record.labels.length) console.log(`  labels: ${record.labels.join(', ')}`);
  console.log(`  node: ${JSON.stringify(record.node)}`);
  console.log(`  ant:  ${JSON.stringify(record.ant)}`);
  if (record.repro) console.log(`  repro: ${path.relative(process.cwd(), record.repro)}`);
}

export function main(argv: readonly string[] = process.argv.slice(2)): number {
  let options: Options;
  try {
    options = parseArgs(argv);
  } catch (error) {
    console.error((error as Error).message);
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
  for (const [label, executable] of [['Node', options.node], ['Ant', options.ant]]) {
    if (!fs.existsSync(executable)) {
      console.error(`${label} executable not found: ${executable}`);
      return 2;
    }
  }

  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-differential-'));
  let mismatches = 0;
  let cases = 0;
  try {
    for (const name of options.families) {
      const record = runFamily(familyNamed(name), options, tempDir);
      cases += record.runs;
      if (!record.match) mismatches += 1;
      printRecord(record, options.json);
    }
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
  if (!options.json) console.log(`\n${options.families.length} families, ${cases} cases, ${mismatches} with mismatches`);
  return mismatches === 0 ? 0 : 1;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) process.exitCode = main();
