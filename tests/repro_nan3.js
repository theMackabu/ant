// Isolate: does the NaN come from the return expression or from sum corruption?

// Test A: upvalue capture, return constant (no sum, no n)
function test_const(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { f(j); }
  return 42;
}

// Test B: upvalue capture, return n (param read)
function test_n(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { f(j); }
  return n;
}

// Test C: upvalue capture, return n * 4
function test_nmul(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { f(j); }
  return n * 4;
}

// Test D: upvalue capture, return sum
function test_sum(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { f(j); }
  return sum;
}

// Test E: upvalue capture but no call to f in loop, return n * 4
function test_nocall(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { sum = sum + 1; }
  return n * 4;
}

var tests = [
  ["const",  test_const,  function(n) { return 42; }],
  ["n",      test_n,      function(n) { return n; }],
  ["n*4",    test_nmul,   function(n) { return n * 4; }],
  ["sum",    test_sum,    function(n) { return n; }],
  ["nocall", test_nocall, function(n) { return n * 4; }],
];

for (var t = 0; t < tests.length; t++) {
  var name = tests[t][0], fn = tests[t][1], expect = tests[t][2];
  var fails = 0;
  for (var i = 0; i < 40; i++) {
    var n = (i < 5) ? (i + 1) : (i * 200);
    var result = fn(n);
    var exp = expect(n);
    if (result !== exp) {
      if (fails < 3)
        console.log("FAIL " + name + "(" + n + ") = " + result + ", expected " + exp);
      fails++;
    }
  }
  if (fails > 0) console.log("  " + name + ": " + fails + " failures");
  else console.log("  " + name + ": PASS");
}
