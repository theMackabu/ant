// string/array call fast paths receive args pointing into vm->stack;
// coercing an object receiver (toString / proxy length trap) runs user JS
// that can grow and realloc that stack, so the helpers must snapshot args
// before coercion (regression: stale-pointer read after realloc)

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

function burnStack(n) {
  return n <= 0 ? 0 : 1 + burnStack(n - 1);
}

function makeDeepToString(str) {
  return {
    toString() {
      burnStack(2000);
      return str;
    }
  };
}

// indexOf: search + position args read after receiver coercion
const idxRecv = makeDeepToString('hello world hello');
assert(String.prototype.indexOf.call(idxRecv, 'hello', 1) === 12,
  'indexOf position arg survives stack growth');
assert(String.prototype.indexOf.call(idxRecv, 'world') === 6,
  'indexOf search arg survives stack growth');

// substring: start + end args read after receiver coercion
const subRecv = makeDeepToString('abcdefgh');
assert(String.prototype.substring.call(subRecv, 2, 5) === 'cde',
  'substring range args survive stack growth');

// array includes generic path: fromIndex read after a proxy length trap
const target = { 0: 'a', 1: 'b', 2: 'c', 3: 'b' };
const proxied = new Proxy(target, {
  get(t, key) {
    if (key === 'length') {
      burnStack(2000);
      return 4;
    }
    return t[key];
  }
});
assert(Array.prototype.includes.call(proxied, 'b', 2) === true,
  'includes fromIndex survives proxy length trap');
assert(Array.prototype.includes.call(proxied, 'a', 1) === false,
  'includes fromIndex excludes earlier elements');

// plain method-call syntax so the dedicated call opcodes are exercised too
const s = 'xyxyxy';
assert(s.indexOf('xy', 1) === 2, 'string receiver indexOf fast path');
assert(s.substring(1, 4) === 'yxy', 'string receiver substring fast path');

console.log('string-call stack realloc tests passed');
