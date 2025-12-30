var arr = [];
for (var i = 0; i < 1024; i++) arr.push(i);

var start = Date.now();
var sum = 0;
for (var i = 0; i < 100000; i++) {
  sum += arr[i % 1024];
}
var end = Date.now();
console.log('100k regular array accesses:', end - start, 'ms');
console.log('sum:', sum);
