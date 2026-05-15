function outer() {
  let beforeOsr;
  let afterOsr;

  {
    let x = 1;
    beforeOsr = () => x;

    for (let i = 0; i < 300; i++) {
      if (i === 250) afterOsr = () => x;
    }
  }

  {
    let x = 77;
    for (let i = 0; i < 10; i++) x += i;
  }

  return [beforeOsr(), afterOsr()];
}

for (let i = 0; i < 8; i++) {
  const result = outer();
  if (result[0] !== 1 || result[1] !== 1) {
    throw new Error(`upvalue mismatch: ${result[0]},${result[1]}`);
  }
}

console.log('jit-osr-existing-open-upvalue: ok');
