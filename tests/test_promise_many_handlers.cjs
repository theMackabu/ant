// a promise runs every handler registered on it, past any small counter width
function assert(cond, msg) {
  if (!cond) throw new Error(msg || 'assertion failed');
}

const N = 70000;
let ran = 0;
let resolve;
const p = new Promise(r => { resolve = r; });
for (let i = 0; i < N; i++) p.then(() => { ran++; });
resolve(1);

setTimeout(() => {
  assert(ran === N, 'ran ' + ran + ' of ' + N + ' handlers');
  console.log('promise many handlers OK');
}, 10);
