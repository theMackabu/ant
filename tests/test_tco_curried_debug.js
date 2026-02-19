const curriedDown = (n) => {
  if (n <= 0) return 'finished';
  return ((x) => curriedDown(x))(n - 1);
};
console.log(curriedDown(10));
