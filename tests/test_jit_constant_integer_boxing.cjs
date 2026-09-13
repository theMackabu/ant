function values(x) {
  return [-128, 0, 1, 9, 2147483647, 2147483648, 4294967295, -0, 1.5, NaN, Infinity, x];
}
const expected = [-128, 0, 1, 9, 2147483647, 2147483648, 4294967295, -0, 1.5, NaN, Infinity];
for (let i = 0; i < 2000; i++) {
  const result = values(i);
  for (let j = 0; j < expected.length; j++) {
    if (!Object.is(result[j], expected[j])) throw new Error('boxing mismatch at ' + j);
  }
  if (result[expected.length] !== i) throw new Error('dynamic slot mismatch');
  result[0] = 42;
}
console.log('constant integer boxing ok');
