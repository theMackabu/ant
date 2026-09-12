// A suspended generator whose activation holds locals and open upvalues that
// nothing else references. Once the generator object is promoted, a minor
// collection no longer scans it, so anything young in its activation was
// freed under it: resuming or destroying the generator then read a freed
// upvalue (segfault at address 0 on the stable build). Captured activations
// now sit in a remembered set that every minor collection scans.
function* g(i) { let x = { i }; const f = () => x; yield f; yield 2; yield 3; }
let live = [];
for (let i = 0; i < 300000; i++) {
  const it = g(i);
  const f = it.next().value;
  if (i % 1000 === 0) live.push(it);
  if (i % 7 === 0) f();
}
setTimeout(() => {
  let s = 0;
  for (const it of live) s += it.next().value;
  if (s !== 600) throw new Error('resumed generators lost state: ' + s);
  console.log('generator-open-upvalue-gc: ok');
}, 0);
