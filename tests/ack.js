function ack(i, j) {
  console.log('ack(' + i + ', ' + j + ')');
  if (i == 0) return j + 1;
  if (j == 0) return ack(i - 1, 1);
  return ack(i - 1, ack(i, j - 1));
}

console.log(ack(3, 3));
