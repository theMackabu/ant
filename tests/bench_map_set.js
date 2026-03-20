var N = 100000;

var m = new Map();
var start = Date.now();
for (var i = 0; i < N; i++) {
  m.set('key' + i, i);
}
var end = Date.now();
console.log('100k Map.set:', end - start, 'ms');

start = Date.now();
var sum = 0;
for (var i = 0; i < N; i++) {
  sum += m.get('key' + i);
}
end = Date.now();
console.log('100k Map.get:', end - start, 'ms');
console.log('sum:', sum);

var s = new Set();
start = Date.now();
for (var i = 0; i < N; i++) {
  s.add('val' + i);
}
end = Date.now();
console.log('100k Set.add:', end - start, 'ms');

start = Date.now();
var hits = 0;
for (var i = 0; i < N; i++) {
  if (s.has('val' + i)) hits++;
}
end = Date.now();
console.log('100k Set.has:', end - start, 'ms');
console.log('hits:', hits);
