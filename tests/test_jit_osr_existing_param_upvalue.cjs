function outer(x) {
  const beforeOsr = () => x;
  let afterOsr;

  for (let i = 0; i < 300; i++) {
    if (i === 250) afterOsr = () => x;
  }

  return [beforeOsr, afterOsr];
}

function churn() {
  let y = 77;
  for (let i = 0; i < 40; i++) y += i;
  return y;
}

for (let i = 0; i < 20; i++) {
  const result = outer(1);
  churn();
  const before = result[0]();
  const after = result[1]();

  if (before !== 1 || after !== 1) {
    throw new Error(`param upvalue mismatch: ${before},${after}`);
  }
}

console.log("jit-osr-existing-param-upvalue: ok");
