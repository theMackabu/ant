const now = () => typeof performance !== 'undefined' && performance.now ? performance.now() : Date.now();

function bench(name, fn, iters = 1) {
  fn();
  const t0 = now();
  for (let i = 0; i < iters; i++) fn();
  const dt = now() - t0;
  const per = (dt / iters).toFixed(3);
  console.log(`${name}: ${dt.toFixed(2)} ms total, ${per} ms/iter (${iters} iters)`);
}

// simple escape sequences
bench('simple escapes (\\n\\t\\r)', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "hello\tworld\nfoo\rbar";
}, 50);

bench('backslash + quote escapes', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "it\'s a \"test\" with \\backslash";
}, 50);

// hex escapes \xNN
bench('hex escapes (\\xNN)', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "\x41\x42\x43\x61\x62\x63\x00\xff";
}, 50);

// unicode escapes \uNNNN
bench('unicode 4-digit (\\uNNNN)', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "\u0041\u00e9\u4e16\u754c\u0048\u0065";
}, 50);

// unicode braced escapes \u{NNNNN}
bench('unicode braced (\\u{N+})', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "\u{41}\u{e9}\u{4e16}\u{1f600}\u{10ffff}";
}, 50);

// null escape
bench('null escape (\\0)', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "abc\0def\0ghi";
}, 50);

// mixed escapes in one string
bench('mixed escapes', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "tab\there\nnewline\r\x41\u0042\u{43}end\\done";
}, 50);

// template literals with escapes
bench('template literal escapes', () => {
  let s = '';
  const x = 42;
  for (let i = 0; i < 5000; i++) s = `\t\n\x41\u0042\u{43} val=${x}`;
}, 50);

// template literal with many interpolations
bench('template interpolation heavy', () => {
  let s = '';
  const a = 1, b = 2, c = 3, d = 4, e = 5;
  for (let i = 0; i < 5000; i++) s = `${a}-${b}-${c}-${d}-${e}`;
}, 50);

// long string with scattered escapes
bench('long string scattered escapes', () => {
  let s = '';
  for (let i = 0; i < 2000; i++) s = "aaaaaaaaaaaaaaaa\nbbbbbbbbbbbbbbbb\tcccccccccccccccc\rdddddddddddddddd\x41eeeeeeeeeeeeeeee\u0042ffffffffffffffff";
}, 50);

// string concatenation with escapes
bench('concat with escapes', () => {
  let s = '';
  for (let i = 0; i < 2000; i++) s += "\n\t\x41\u0042";
}, 20);

// octal escapes (legacy)
bench('octal escapes', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "\101\102\103\141\142\143";
}, 50);

// form feed, vertical tab, backspace
bench('rare escapes (\\v\\f\\b)', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "a\vb\fc\bd";
}, 50);

// no escapes baseline
bench('no escapes (baseline)', () => {
  let s = '';
  for (let i = 0; i < 5000; i++) s = "hello world this is a plain string with no escapes at all";
}, 50);
