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
function boxedWords(value) {
  return [value | 0, value >>> 0, ~value];
}
function receiveWords(signed, unsigned, inverted) {
  return [signed, unsigned, inverted];
}
function callWithWords(value) {
  return receiveWords(value | 0, value >>> 0, ~value);
}
const wordCases = [
  [0, 0, 0, -1], [-0, 0, 0, -1], [-1, -1, 4294967295, 0],
  [2147483647, 2147483647, 2147483647, -2147483648],
  [2147483648, -2147483648, 2147483648, 2147483647],
  [4294967295, -1, 4294967295, 0], [4294967296, 0, 0, -1],
  [-2147483648, -2147483648, 2147483648, 2147483647],
  [1.75, 1, 1, -2], [NaN, 0, 0, -1], [Infinity, 0, 0, -1],
];
for (let round = 0; round < 800; round++) {
  for (const [value, ...words] of wordCases) {
    for (const result of [boxedWords(value), callWithWords(value)]) {
      for (let i = 0; i < words.length; i++) {
        if (!Object.is(result[i], words[i])) throw new Error('word boxing mismatch at ' + i);
      }
    }
  }
}
console.log('constant and dynamic integer boxing ok');
