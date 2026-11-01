// Runs a probe under one engine and compares complete process results.

import childProcess from 'node:child_process';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

export const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');

export interface EngineResult {
  status: number | null;
  signal: NodeJS.Signals | null;
  timedOut: boolean;
  stdout: string;
  stderr: string;
  spawnError: string | null;
}

// An Ant configuration a probe must also match Node under. A variant with
// `stderrFailures` traces to stderr: its stderr is not compared, but any
// line matching that pattern is a mismatch.
export interface Variant {
  name: string;
  args?: readonly string[];
  env?: Readonly<Record<string, string>>;
  stderrFailures?: string;
}

export const defaultVariants: readonly Variant[] = [{ name: 'default' }];

const normalizeNewlines = (value: string | null | undefined): string => String(value || '').replace(/\r\n/g, '\n');

export function runEngine(executable: string, sourceFile: string, timeout: number, variant: Variant = { name: 'default' }): EngineResult {
  const env: NodeJS.ProcessEnv = { ...process.env, ...variant.env, NO_COLOR: '1', FORCE_COLOR: '0' };
  if (!variant.env || !('ANT_DEBUG' in variant.env)) delete env.ANT_DEBUG;
  const result = childProcess.spawnSync(executable, [...(variant.args || []), sourceFile], {
    cwd: repoRoot,
    encoding: 'utf8',
    timeout,
    maxBuffer: 4 * 1024 * 1024,
    env
  });
  const code = (result.error as NodeJS.ErrnoException | undefined)?.code;
  return {
    status: result.status,
    signal: result.signal,
    timedOut: code === 'ETIMEDOUT',
    stdout: normalizeNewlines(result.stdout),
    stderr: normalizeNewlines(result.stderr),
    spawnError: result.error && code !== 'ETIMEDOUT' ? `${code}: ${result.error.message}` : null
  };
}

export function sameResult(left: Partial<EngineResult>, right: Partial<EngineResult>): boolean {
  return JSON.stringify(left) === JSON.stringify(right);
}

export function sameVariantResult(node: EngineResult, ant: EngineResult, variant: Variant): boolean {
  if (!variant.stderrFailures) return sameResult(node, ant);
  if (new RegExp(variant.stderrFailures, 'i').test(ant.stderr)) return false;
  return sameResult({ ...node, stderr: '' }, { ...ant, stderr: '' });
}

// Labels whose recorded values differ, for triage.
export function differingLabels(node: EngineResult, ant: EngineResult, limit = 8): string[] {
  let left: [string, unknown][], right: [string, unknown][];
  try {
    left = JSON.parse(node.stdout);
    right = JSON.parse(ant.stdout);
  } catch {
    return node.stdout === ant.stdout ? [] : ['<stdout>'];
  }
  const values = new Map(right.map(([label, value]) => [label, JSON.stringify(value)]));
  const labels = left.filter(([label, value]) => values.get(label) !== JSON.stringify(value)).map(([label]) => label);
  if (left.length !== right.length && labels.length === 0) labels.push('<record count>');
  return labels.slice(0, limit);
}
