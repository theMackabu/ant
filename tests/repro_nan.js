var global_res;

function closure_var(n) {
  function f(a) {
    sum++;
  }

  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) {
    f(j);
    f(j);
    f(j);
    f(j);
  }
  global_res = sum;
  return n * 4;
}

// Warm up and then check for NaN
for (var i = 0; i < 100; i++) {
  var n = (i < 10) ? (i + 1) : (i * 100);
  var result = closure_var(n);
  var expected = n * 4;
  if (result !== expected) {
    console.log("FAIL: closure_var(" + n + ") = " + result + ", expected " + expected + " (iter " + i + ")");
  }
}
console.log("done");
