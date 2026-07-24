import { test, testDeep, summary } from './helpers.js';

console.log('RegExp Tests\n');

const re1 = new RegExp('hello');
test('RegExp source', re1.source, 'hello');
test('RegExp flags empty', re1.flags, '');
test('RegExp global false', re1.global, false);

const re2 = new RegExp('test', 'g');
test('RegExp global flag', re2.global, true);
test('RegExp flags g', re2.flags, 'g');

const re3 = new RegExp('pattern', 'gi');
test('RegExp multiple flags', re3.flags, 'gi');
test('RegExp global true', re3.global, true);
test('RegExp ignoreCase true', re3.ignoreCase, true);

const re4 = /hello/;
test('literal source', re4.source, 'hello');

const re5 = /test/gi;
test('literal flags', re5.flags, 'gi');

test('test true', /hello/.test('hello world'), true);
test('test false', /hello/.test('goodbye world'), false);
test('test case sensitive', /Hello/.test('hello'), false);
test('test case insensitive', /Hello/i.test('hello'), true);

testDeep('match simple', 'hello world'.match(/world/), ['world']);
testDeep('match groups', 'hello'.match(/(h)(e)/), ['he', 'h', 'e']);

test('exec not null', /o/.exec('hello') !== null, true);
test('exec match', /o/.exec('hello')[0], 'o');

let regexpPrototypeExecThrows = false;
try {
  RegExp.prototype.exec();
} catch (e) {
  regexpPrototypeExecThrows = true;
}
test('RegExp prototype exec incompatible receiver', regexpPrototypeExecThrows, true);

test('search found', 'hello world'.search(/world/), 6);
test('search not found', 'hello world'.search(/xyz/), -1);

test('replace', 'hello world'.replace(/world/, 'there'), 'hello there');
test('replace global', 'a-b-c'.replace(/-/g, '_'), 'a_b_c');

testDeep('split', 'a,b,c'.split(/,/), ['a', 'b', 'c']);

const re6 = new RegExp('multi', 'm');
test('multiline flag', re6.multiline, true);

const re7 = /foo/s;
test('dotAll flag', re7.dotAll, true);

test('sticky flag', /test/y.sticky, true);

// legacy statics reflect the last separator match after split, on both
// the generic path and the internal fast path
'a,b;c'.split(/([,;])/);
test('split updates RegExp.$1', RegExp.$1, ';');
test('split updates RegExp.lastMatch', RegExp.lastMatch, ';');
'x1y22z'.split(/(\d+)/);
test('split statics track later match', RegExp.$1, '22');

// a custom exec's index is integer-converted and clamped before the
// replacer callback sees it
function execWithIndex(idx) {
  class R extends RegExp {
    exec() {
      if (this.done) return null;
      this.done = true;
      const m = ['b'];
      m.index = idx;
      return m;
    }
  }
  return new R('b', 'g');
}
function replacerPosition(idx) {
  let got;
  'abcd'.replace(execWithIndex(idx), (...args) => { got = args[args.length - 2]; return ''; });
  return got;
}
test('replacer index truncated', replacerPosition(1.5), 1);
test('replacer index clamped low', replacerPosition(-3), 0);
test('replacer index clamped high', replacerPosition(99), 4);
test('replacer index NaN is zero', replacerPosition(NaN), 0);
test('replacer index coerced', replacerPosition('2'), 2);

summary();
