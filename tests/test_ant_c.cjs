const assert = require('assert');

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
assert.strictEqual(bits(5), 0x01000101);
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

console.log('Ant.unsafe.c tests passed');
