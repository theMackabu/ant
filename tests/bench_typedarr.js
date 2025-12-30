var arr = new Uint32Array(0x1000 / 4);
arr.fill(0x51515151);

var start = Date.now();
var sum = 0;
for (var i = 0; i < 100000; i++) {
  sum += arr[i % 1024];
}
var end = Date.now();
console.log('100k typed array accesses:', end - start, 'ms');
console.log('sum:', sum);
