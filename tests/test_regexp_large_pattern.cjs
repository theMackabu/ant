function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const literalCount = 5000;
const literalPattern = new RegExp(`^${'a'.repeat(literalCount)}$`);
assert(
  !literalPattern.test('a'.repeat(literalCount - 500)),
  'large literal regexp matched after its end anchor was truncated'
);
assert(
  literalPattern.test('a'.repeat(literalCount)),
  'large literal regexp did not match its complete subject'
);

const combiningRange = '\\u0300-\\u0345';
const expandedPattern = new RegExp(
  `^[A-Za-z${combiningRange.repeat(300)}]+$`
);
// Mirrors pcre2_pattern_stack in compiled_regex_cache_get_or_compile.
const regexpTranslationStackSize = 4096;
assert(
  expandedPattern.source.length < regexpTranslationStackSize,
  'translated-pattern regression must begin below the stack-buffer limit'
);
assert(
  expandedPattern.test('html'),
  'regexp translation truncated a pattern that expanded past the stack buffer'
);

const delimiter = 'z'.repeat(700);
const split = `left${delimiter}right`.split(new RegExp(delimiter));
assert(
  split.length === 2 && split[0] === 'left' && split[1] === 'right',
  'large regexp split delimiter was truncated'
);

assert(
  new RegExp('[^&&b]', 'v').test('b'),
  'bare ^ v-set operand no longer matches the legacy translation'
);

console.log('large regexp pattern tests passed');
