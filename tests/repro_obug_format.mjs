import { formatWithOptions } from 'node:util';
import process from 'node:process';

function humanize(value) {
  if (value >= 1000) return `${(value / 1000).toFixed(1)}s`;
  return `${value}ms`;
}

function writeDebugLine(namespace, args, diff) {
  const prefix = `  \u001B[32;1m${namespace} \u001B[0m`;
  args[0] = prefix + args[0].split('\n').join(`\n${prefix}`);
  args.push(`\u001B[32m+${humanize(diff)}\u001B[0m`);
  return formatWithOptions({}, ...args);
}

const raw = formatWithOptions({}, '%s %s : %s', 'fn', '/', '/foo.png');
const debugLine = writeDebugLine('repro:test', ['%s %s : %s', 'fn', '/', '/foo.png'], 0);

console.log('raw:');
console.log(raw);
console.log('debug:');
console.log(debugLine);

if (raw !== 'fn / : /foo.png') {
  throw new Error(`util.formatWithOptions mismatch: ${JSON.stringify(raw)}`);
}

if (!debugLine.includes('fn / : /foo.png')) {
  throw new Error(`obug-style line mismatch: ${JSON.stringify(debugLine)}`);
}

process.exit(0);
