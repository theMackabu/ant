// Minimal repro: JIT bailout on string ADD inside a hot callback
function test() {
  const prefix = 'hello';
  const arr = [1, 2, 3, 4, 5];

  // Callback captures `prefix` as upvalue and does string + string
  const result = arr.map((v) => {
    return prefix + String(v);
  });

  return result;
}

// Trigger JIT: 5 items * 21 calls = 105 > threshold(100)
for (let i = 0; i < 25; i++) {
  const r = test();
  if (i === 24) console.log('OK:', r.join(', '));
}
