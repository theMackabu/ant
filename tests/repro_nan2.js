// Test 1: closure_var with upvalue capture (original bug)
function closure_var(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { f(j); f(j); f(j); f(j); }
  return n * 4;
}

// Test 2: same loop but no upvalue capture - just a noop inner call
function no_capture(n) {
  function f(a) { return 1; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { sum += f(j); }
  return n;
}

// Test 3: same structure but no inner call at all
function no_inner_call(n) {
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { sum += 1; sum += 1; sum += 1; sum += 1; }
  return n * 4;
}

// Test 4: closure_var but return sum instead of n*4
function closure_ret_sum(n) {
  function f(a) { sum++; }
  var j, sum;
  sum = 0;
  for (j = 0; j < n; j++) { f(j); f(j); f(j); f(j); }
  return sum;
}

var tests = [
  ["closure_var", closure_var, function(n) { return n * 4; }],
  ["no_capture", no_capture, function(n) { return n; }],
  ["no_inner_call", no_inner_call, function(n) { return n * 4; }],
  ["closure_ret_sum", closure_ret_sum, function(n) { return n * 4; }],
];

for (var t = 0; t < tests.length; t++) {
  var name = tests[t][0], fn = tests[t][1], expect = tests[t][2];
  var fails = 0;
  for (var i = 0; i < 60; i++) {
    var n = (i < 10) ? (i + 1) : (i * 100);
    var result = fn(n);
    var exp = expect(n);
    if (result !== exp) {
      if (fails < 5)
        console.log("FAIL " + name + "(" + n + ") = " + result + ", expected " + exp);
      fails++;
    }
  }
  if (fails > 0) console.log("  " + name + ": " + fails + " failures total");
  else console.log("  " + name + ": PASS");
}
