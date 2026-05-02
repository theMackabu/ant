const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-ts-type-hints-'));
const scriptPath = path.join(tmpRoot, 'entry.ts');

fs.writeFileSync(
  scriptPath,
  [
    'function add(a: number, b: number): number {',
    '  return a + b;',
    '}',
    '',
    'function observedMismatch(a: number, b: number): number {',
    '  return a + b;',
    '}',
    '',
    'let total = 0;',
    'for (let i = 0; i < 160; i++) total += add(i, 1);',
    'console.log(total);',
    'console.log(add("x" as any, "y" as any));',
    'console.log(observedMismatch("pre" as any, "jit" as any));',
    'console.log(observedMismatch("type" as any, "feedback" as any));',
    'let mismatchTotal = 0;',
    'for (let i = 0; i < 160; i++) mismatchTotal += observedMismatch(i, 2);',
    'console.log(mismatchTotal);',
    'console.log(observedMismatch("post" as any, "warmup" as any));',
    '',
  ].join('\n')
);

const result = spawnSync(process.execPath, [scriptPath], {
  encoding: 'utf8',
});

if (result.error) throw result.error;

const stdout = result.stdout.replace(/\x1b\[[0-9;]*m/g, '');

assert(
  result.status === 0,
  `expected TypeScript type-hint optimization to preserve runtime semantics, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
);
assert(
  stdout === '12880\nxy\nprejit\ntypefeedback\n13040\npostwarmup\n',
  `expected numeric warmup and string fallback output, got ${JSON.stringify(stdout)}`
);

console.log('TypeScript type hints preserve guarded runtime semantics');
