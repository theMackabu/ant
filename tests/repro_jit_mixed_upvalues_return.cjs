function factory(prefix) {
  return function handle(n) {
    let count = n + 1;
    let state = { value: prefix + ':' + n };
    let items = [prefix, n];

    function trimPrefix(x) {
      return x + ':' + items[0];
    }

    function next() {
      return state.value + '|' + count + '|' + trimPrefix('ok');
    }

    return next;
  };
}

const handle = factory('root');

for (let i = 0; i < 150; i++) {
  const fn = handle(i);
  if (fn() !== `root:${i}|${i + 1}|ok:root`) {
    throw new Error(`warmup mismatch at ${i}: ${fn()}`);
  }
}

const next = handle(7);
const got = next();
const want = 'root:7|8|ok:root';
if (got !== want) {
  throw new Error(`expected ${want}, got ${got}`);
}

console.log('OK: mixed upvalue returned closure survived', got);
