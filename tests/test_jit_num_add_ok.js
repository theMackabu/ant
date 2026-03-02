// Control test: JIT ADD with numbers only (no bailout) — should NOT crash
function test() {
  const arr = [1, 2, 3, 4, 5];
  const result = arr.map((v) => {
    return v + 10;
  });
  return result;
}

for (let i = 0; i < 25; i++) {
  const r = test();
  if (i === 24) console.log('OK:', r.join(', '));
}
