const { parseArgs } = require('node:util');

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

let result = parseArgs({
  args: ['--prompt', 'hello', '-c', 'positional'],
  options: {
    prompt: { type: 'string', short: 'p' },
    continue: { type: 'boolean', short: 'c', default: false },
    format: { type: 'string', default: 'default' }
  },
  strict: false,
  allowPositionals: true
});

assert(result.values.prompt === 'hello', 'expected string long option');
assert(result.values.continue === true, 'expected boolean short option');
assert(result.values.format === 'default', 'expected default string option');
assert(Array.isArray(result.positionals), 'expected positionals array');
assert(result.positionals.length === 1 && result.positionals[0] === 'positional', 'expected positional passthrough');

result = parseArgs({
  args: ['-phello', '--format=json'],
  options: {
    prompt: { type: 'string', short: 'p' },
    format: { type: 'string' }
  },
  strict: false,
  allowPositionals: true
});

assert(result.values.prompt === 'hello', 'expected attached short string value');
assert(result.values.format === 'json', 'expected inline long string value');

console.log('PASS');
