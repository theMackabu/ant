const assert = require('assert');
const { spawnSync } = require('child_process');

function runRejectedEntry(source) {
  const result = spawnSync(process.execPath, ['-e', source], { encoding: 'utf8' });
  if (result.error) throw result.error;
  assert.strictEqual(result.signal, null, result.stderr || result.stdout);
  assert.notStrictEqual(result.status, 0, 'mismatched C entry signature was accepted');
  return result.stderr;
}

assert.strictEqual(
  Ant.unsafe.c`
    #include <stdio.h>

    int main(void) {
      printf("hello from C\n");
      return 7;
    }
  `,
  7,
);

assert.strictEqual(
  Ant.unsafe.c`int main(int argc, char **argv) { return argc + (argv != 0); }`,
  2,
);

assert.strictEqual(
  Ant.unsafe.c`
    int main(int argc, char **argv, char **envp) {
      return argc + (argv != 0) + (envp == 0);
    }
  `,
  3,
);

assert.throws(
  () => Ant.unsafe.c`int value = ;`,
  /Ant\.unsafe\.c\(\) compilation failed/,
);

assert.throws(
  () => Ant.unsafe.c`int answer(void) { return 42; }`,
  /Ant\.unsafe\.c\(\) source must define main\(\)/,
);

assert.strictEqual(
  Ant.unsafe.c({ entry: 'result', returns: 'string' })`
    const char *result(void) { return "hello from C"; }
  `,
  'hello from C',
);

assert.strictEqual(
  Ant.unsafe.c({ entry: 'result', returns: 'string' })`
    const char *result(void) { return 0; }
  `,
  null,
);

const describeNumber = Ant.unsafe.c({
  entry: 'describe_number',
  args: ['double'],
  returns: 'string',
})`
  const char *describe_number(double value) { return value > 0 ? "positive" : "other"; }
`;
assert.strictEqual(describeNumber(1), 'positive');

assert.throws(
  () => Ant.unsafe.c({ entry: 'not-an-identifier', returns: 'int' })`
    int main(void) { return 0; }
  `,
  /options\.entry must be a C identifier/,
);

assert.throws(
  () => Ant.unsafe.c({ entry: 'missing', returns: 'string' })`
    int main(void) { return 0; }
  `,
  /Ant\.unsafe\.c\(\) entry "missing" was not found/,
);

assert.throws(
  () => Ant.unsafe.c({ entry: 'result', returns: 'string' })`
    int result(void) { return 42; }
  `,
  /must return char \*/,
);

assert.throws(
  () => Ant.unsafe.c`char *main(void) { return "not an exit status"; }`,
  /main must return int/,
);

const bits = Ant.unsafe.c({
  entry: 'unpack_bits',
  args: ['uint8'],
  returns: 'uint32',
})`
  #include <stdint.h>

  uint32_t unpack_bits(uint8_t num) {
    uint32_t x = num;
    x = (x | (x << 12)) & 0x000F000F;
    x = (x | (x <<  6)) & 0x03030303;
    x = (x | (x <<  3)) & 0x11111111;
    return x;
  }
`;

assert.strictEqual(typeof bits, 'function');
assert.strictEqual(bits(5), 0x00000101);
assert.strictEqual(bits(0xA5), 0x10100101);
assert.throws(() => bits(), /expects 1 arguments, got 0/);
assert.throws(() => bits('5'), /argument 1 must be a number/);

const interpolatedStatus = 11;
assert.strictEqual(
  Ant.unsafe.c`int main(void) { return ${interpolatedStatus}; }`,
  interpolatedStatus,
);

assert.throws(
  () => Ant.unsafe.c('int main(void) { return 0; }'),
  /must be used as a tagged template or called with options/,
);

assert.strictEqual(
  Ant.unsafe.c({ entry: 'result', returns: 'int8' })`
    #include <stdint.h>
    int8_t result(void) { return -12; }
  `,
  -12,
);

assert.strictEqual(
  Ant.unsafe.c({ entry: 'result', returns: 'double' })`
    double result(void) { return 1.5; }
  `,
  1.5,
);

const negativeInt64 = Ant.unsafe.c({ entry: 'result', returns: 'int64' })`
    #include <stdint.h>
    int64_t result(void) { return -9007199254740993LL; }
  `;
assert.strictEqual(negativeInt64, -9007199254740993n);

const addUint64 = Ant.unsafe.c({
  entry: 'add',
  args: ['uint64', 'uint64'],
  returns: 'uint64',
})`
  #include <stdint.h>
  uint64_t add(uint64_t left, uint64_t right) { return left + right; }
`;
assert.strictEqual(addUint64(9007199254740993n, 7n), 9007199254741000n);
assert.throws(() => addUint64(1, 2n), /argument 1 must be a bigint/);

assert.match(
  runRejectedEntry(`
    const template = ['unsigned long long result(void) { return 1; }'];
    template.raw = template;
    Ant.unsafe.c({ entry: 'result', returns: 'string' })(template);
  `),
  /string entry must return char \*/,
);

assert.match(
  runRejectedEntry(`
    const template = ['unsigned long long convert(unsigned long long value) { return value; }'];
    template.raw = template;
    Ant.unsafe.c({ entry: 'convert', args: ['double'], returns: 'uint64' })(template);
  `),
  /must match configured signature/,
);

console.log('Ant.unsafe.c tests passed');
