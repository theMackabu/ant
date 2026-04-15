const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tmpRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-tla-dead-await-fastpath-'));
const modulePath = path.join(tmpRoot, 'bench.mjs');

fs.writeFileSync(
  modulePath,
  [
    'const now =',
    "  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'",
    '    ? () => performance.now()',
    '    : () => Date.now();',
    '',
    'function getColorEnabled() {',
    '  const { process, Deno } = globalThis;',
    "  const isNoColor = typeof Deno?.noColor === 'boolean'",
    '    ? Deno.noColor',
    "    : process !== void 0 ? 'NO_COLOR' in process?.env : false;",
    '  return !isNoColor;',
    '}',
    '',
    'const ua = globalThis.navigator?.userAgent ?? "";',
    'if (!globalThis.navigator || ua === "Cloudflare-Workers") {',
    '  console.log(JSON.stringify({ skipped: true, ua }));',
    '  process.exit(0);',
    '}',
    '',
    'const iterations = Number(process.env.ANT_TLA_DEAD_AWAIT_ITERS || 20000);',
    '',
    'const guardStart = now();',
    'for (let i = 0; i < iterations; i++) {',
    '  const isNoColor =',
    '    navigator !== void 0 && navigator.userAgent === "Cloudflare-Workers"',
    '      ? false',
    '      : !getColorEnabled();',
    '  void isNoColor;',
    '}',
    'const guardMs = now() - guardStart;',
    '',
    'const deadStart = now();',
    'for (let i = 0; i < iterations; i++) {',
    '  const isNoColor =',
    '    navigator !== void 0 && navigator.userAgent === "Cloudflare-Workers"',
    '      ? await (async () => false)()',
    '      : !getColorEnabled();',
    '  void isNoColor;',
    '}',
    'const deadMs = now() - deadStart;',
    '',
    'console.log(JSON.stringify({',
    '  skipped: false,',
    '  ua,',
    '  iterations,',
    '  guardMs,',
    '  deadMs,',
    '  guardUsPerOp: (guardMs * 1000) / iterations,',
    '  deadUsPerOp: (deadMs * 1000) / iterations,',
    '}));',
    '',
  ].join('\n')
);

const result = spawnSync(process.execPath, [modulePath], {
  encoding: 'utf8',
});

if (result.error) throw result.error;
assert(
  result.status === 0,
  `expected module benchmark to exit 0, got ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`
);

const output = result.stdout.trim();
assert(output.length > 0, `expected benchmark JSON output, got stdout=${JSON.stringify(result.stdout)}`);

const parsed = JSON.parse(output);
console.log(`navigator.userAgent=${parsed.ua || '<missing>'}`);

if (parsed.skipped) {
  console.log('skipping TLA dead-await fast-path regression outside the normal fast path');
  process.exit(0);
}

const ratioLimit = Number(process.env.ANT_TLA_DEAD_AWAIT_RATIO_LIMIT || 6);
const ratio = parsed.deadUsPerOp / parsed.guardUsPerOp;

console.log(
  `guard=${parsed.guardUsPerOp.toFixed(3)}us/op dead=${parsed.deadUsPerOp.toFixed(3)}us/op ratio=${ratio.toFixed(2)}x`
);

assert(
  ratio <= ratioLimit,
  `untaken TLA await branch regressed the fast path (ratio ${ratio.toFixed(2)}x, limit ${ratioLimit}x)`
);
