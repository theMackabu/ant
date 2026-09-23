/*
 * Native-stack boundary micro-bench.
 *
 * Each case crosses one kind of boundary between C, the interpreter and
 * compiled code many times, to measure what the crossing itself costs:
 *   c_to_interp    C calls into the interpreter (builtin -> interpreted callback)
 *   interp_to_jit  the interpreter calls compiled code
 *   interp_c_jit   C calls compiled code while the interpreter is innermost
 *   jit_c_jit      compiled code calls C that calls compiled code (constructors)
 *   pure_jit       control: compiled code with no crossings
 *
 * The JIT never compiles async functions, so async functions without an
 * await run their bodies in the interpreter, synchronously. The
 * c_to_interp callback is async too, so that case also pays for one promise
 * per call; compare it across builds, not against the other cases.
 */

function get_clock() {
  if (typeof performance !== 'undefined' && performance.now) return performance.now();
  return Date.now();
}

var N = 2000000;
var arr = [];
for (var k = 0; k < 1000; k++) arr.push(k);

function jit_add(a, b) { return a + b; }
function Point(x, y) { this.x = x; this.y = y; }

function c_to_interp() {
  var s = 0;
  var cb = async function (x) { s += x; };
  for (var r = 0; r < N / 1000; r++) arr.forEach(cb);
  return s;
}

// async bodies run synchronously up to their first await, so the result is
// ready when the call returns; it goes through a variable rather than .then,
// which would only run as a later microtask
var sync_out = 0;

async function interp_to_jit_body() {
  var s = 0;
  for (var i = 0; i < N; i++) s = jit_add(s, i);
  sync_out = s;
}

async function interp_c_jit_body() {
  var s = 0;
  var cb = function (x) { s += x; };
  for (var r = 0; r < N / 1000; r++) arr.forEach(cb);
  sync_out = s;
}

function interp_to_jit() { interp_to_jit_body(); return sync_out; }
function interp_c_jit() { interp_c_jit_body(); return sync_out; }

function jit_c_jit() {
  var s = 0;
  for (var i = 0; i < N; i++) { var p = new Point(i, 1); s += p.x; }
  return s;
}

function pure_jit() {
  var s = 0;
  for (var i = 0; i < N; i++) s = (s + i * 3) % 1000003;
  return s;
}

function run_case(name, fn) {
  fn();
  var t0 = get_clock();
  var result = fn();
  var dt = get_clock() - t0;
  console.log(name + ': ' + dt.toFixed(2) + ' ms (checksum ' + result + ')');
}

function main() {
  for (var i = 0; i < 5000; i++) jit_add(i, i);
  run_case('c_to_interp', c_to_interp);
  run_case('interp_to_jit', interp_to_jit);
  run_case('interp_c_jit', interp_c_jit);
  run_case('jit_c_jit', jit_c_jit);
  run_case('pure_jit', pure_jit);
}

main();
