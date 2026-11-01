import childProcess from 'node:child_process';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

export const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
export const defaultVariants = [{ name: 'default' }];

const normalizeNewlines = value => String(value || '').replace(/\r\n/g, '\n');

export function runEngine(executable, sourceFile, timeout, variant = { name: 'default' }) {
  const env = { ...process.env, ...variant.env, NO_COLOR: '1', FORCE_COLOR: '0' };
  if (!variant.env || !('ANT_DEBUG' in variant.env)) delete env.ANT_DEBUG;
  const result = childProcess.spawnSync(executable, [...(variant.args || []), sourceFile], {
    cwd: repoRoot,
    encoding: 'utf8',
    timeout,
    maxBuffer: 4 * 1024 * 1024,
    env
  });
  const code = result.error?.code;
  return {
    status: result.status,
    signal: result.signal,
    timedOut: code === 'ETIMEDOUT',
    stdout: normalizeNewlines(result.stdout),
    stderr: normalizeNewlines(result.stderr),
    spawnError: result.error && code !== 'ETIMEDOUT' ? `${code}: ${result.error.message}` : null
  };
}

export function sameResult(left, right) {
  return JSON.stringify(left) === JSON.stringify(right);
}

export function sameVariantResult(node, ant, variant) {
  if (!variant.stderrFailures) return sameResult(node, ant);
  if (new RegExp(variant.stderrFailures, 'i').test(ant.stderr)) return false;
  return sameResult({ ...node, stderr: '' }, { ...ant, stderr: '' });
}

export function differingLabels(node, ant, limit = 8) {
  let left, right;
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
