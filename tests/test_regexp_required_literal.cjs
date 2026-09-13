function same(actual, expected, message) {
  if (!Object.is(actual, expected)) throw new Error(message + ': ' + actual + ' / ' + expected);
}

const pad = '~'.repeat(160);
const cases = [
  ['(?:a|b)?marker', 'marker'],
  ['(?:marker)?other', 'other'],
  ['(?:prefix|suffix)long(?:x|y)', 'prefixlongx'],
  ['(?:start|)optional?', 'optiona'],
  ['(?:start|)abcdefgh{0}ijk', 'abcdefgijk'],
  ['(?:start|)abcdefgh{0,2}ijk', 'abcdefgijk'],
  ['(?:start|)abcdefgh*ijk', 'abcdefgijk'],
  ['(?:start|)abcdefgh+ijk', 'abcdefghijk'],
  ['(?:start|)abcdefgh?ijk', 'abcdefgijk'],
  ['(?:x|y)mandatory|alternate', 'alternate'],
  ['((a|b)(c|d))?required', 'required'],
  ['[^x]*target', 'target'],
  ['[^\\]]*target', 'target'],
  ['[^\\[]*target', 'target'],
  ['(?:x|y)?\\.literal\\(a\\)', '.literal(a)'],
  ['(?:x|y)?abcdefghijklmnopqrstuvwxyz0123456789?', 'abcdefghijklmnopqrstuvwxyz012345678'],
  ['(?:x|y)?\\u0061lpha', 'alpha'],
  ['(?:x|y)?\\x61lpha', 'alpha'],
  ['(?:x|y)?(alpha)\\1', 'alphaalpha'],
  ['(?=alpha)alpha', 'alpha'],
  ['(?<name>alpha)', 'alpha'],
  ['(?<=before)alpha', 'beforealpha'],
  ['(?:é|😀)?marker', 'émarker'],
  ['(?:x|y)?a\0marker', 'a\0marker']
];
for (const [pattern, tail] of cases) {
  const rx = new RegExp(pattern, 'g');
  const subject = pad + tail;
  for (let i = 0; i < 40; i++) {
    rx.lastIndex = 0;
    const result = rx.exec(subject);
    same(result !== null, true, 'required match ' + pattern);
  }
}

const rx = /(^|[^\\])"\\\/Date\((-?[0-9]+)\)\\\/"/g;
const match = pad + '"\\/Date(-42)\\/"';
const miss = pad + '"Date":42';
same(rx.test(miss), false, 'missing required literal');
same(rx.lastIndex, 0, 'failure resets lastIndex');
const result = rx.exec(match);
same(result[2], '-42', 'capture after prefilter hit');
same(rx.lastIndex, match.length, 'global success lastIndex');
same(rx.exec(match), null, 'search begins at lastIndex');
same(rx.lastIndex, 0, 'end-of-subject failure resets lastIndex');
same(match.replace(rx, '$2'), pad.slice(0, -1) + '-42', 'replacement preserves captures');
same(miss.replace(rx, 'bad'), miss, 'no-match replacement');

const sticky = /(?:x|y)?marker/y;
sticky.lastIndex = pad.length;
same(sticky.exec(pad + 'marker')[0], 'marker', 'sticky match');
sticky.lastIndex = pad.length - 1;
same(sticky.exec(pad + 'marker'), null, 'literal hit does not bypass anchoring');
same(new RegExp('(?:x|y)?marker', 'i').test(pad + 'MARKER'), true, 'case-folding fallback');
same(new RegExp('(?:x|y)?[[a-z]&&[^q]]+', 'v').test(pad + 'marker'), true, 'set syntax fallback');

// Every expression has a known witness. Combining boundaries on both sides
// catches false rejection when a group, class, escape or quantifier breaks a
// literal run. No reference-engine artifact is needed for these assertions.
const fragments = [
  ['(x|)', ''], ['(?:abc)?', ''], ['(?:x|y)*', 'xy'], ['[xy]+', 'xy'],
  ['[^x]{2}', 'zz'], ['\\b', ''], ['\\d*', '12'], ['\\.', '.'],
  ['abcdefgh?', 'abcdefg'], ['abcdefgh{0}', 'abcdefg'],
  ['abcdefgh{0,2}?', 'abcdefg'], ['abcdefghijklmnopqrstuvwx0123456789*', 'abcdefghijklmnopqrstuvwx012345678']
];
for (const [left, leftText] of fragments) {
  for (const [right, rightText] of fragments) {
    const pattern = left + 'MIDDLE' + right;
    // Word boundaries need punctuation on the adjacent side of MIDDLE.
    if (left === '\\b' || right === '\\b') continue;
    same(new RegExp(pattern).test(pad + leftText + 'MIDDLE' + rightText), true, pattern);
  }
}

console.log('regexp required literal ok');
