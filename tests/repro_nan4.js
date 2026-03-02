// Test: many SHORT calls to trigger call_count JIT (not OSR back-edge)
function test_short(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { f(j); }
  return n;
}

// Call with small n many times to trigger call_count threshold
var fails = 0;
for (var i = 0; i < 200; i++) {
  var result = test_short(5);
  if (result !== 5) {
    if (fails < 3) console.log("FAIL iter " + i + ": test_short(5) = " + result);
    fails++;
  }
}
console.log("call_count path: " + (fails > 0 ? fails + " failures" : "PASS"));

// Now test with large n to trigger OSR
fails = 0;
for (var i = 0; i < 20; i++) {
  var n = 2000;
  var result = test_short(n);
  if (result !== n) {
    if (fails < 3) console.log("FAIL iter " + i + ": test_short(" + n + ") = " + result);
    fails++;
  }
}
console.log("OSR path: " + (fails > 0 ? fails + " failures" : "PASS"));
