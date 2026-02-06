/*
 * Array creation micro-bench
 */

function get_clock() {
  if (typeof performance !== 'undefined' && performance.now) return performance.now();
  return Date.now();
}

function run_case(name, fn, iters) {
  var i;
  var t0 = get_clock();
  for (i = 0; i < iters; i++) fn();
  var dt = get_clock() - t0;
  console.log(name + ': ' + dt.toFixed(2) + ' ms');
}

function create_assign() {
  var tab = [];
  for (var i = 0; i < 1000; i++) tab[i] = i;
}

function create_push() {
  var tab = [];
  for (var i = 0; i < 1000; i++) tab.push(i);
}

function create_prealloc_assign() {
  var tab = [];
  tab.length = 1000;
  for (var i = 0; i < 1000; i++) tab[i] = i;
}

function create_prealloc_push() {
  var tab = [];
  tab.length = 1000;
  for (var i = 0; i < 1000; i++) tab[i] = i;
}

function main() {
  var iters = 200;
  run_case('create_assign', create_assign, iters);
  run_case('create_push', create_push, iters);
  run_case('create_prealloc_assign', create_prealloc_assign, iters);
  run_case('create_prealloc_push', create_prealloc_push, iters);

  var arr = [];
  var t0 = get_clock();
  for (var i = 0; i < 20000; i++) {
    arr.push(i);
  }
  var dt = get_clock() - t0;
  console.log('push_20k: ' + dt.toFixed(2) + ' ms');
  console.log('done, arr length: ' + arr.length);
  console.log('first element: ' + arr[0]);
  console.log('last element: ' + arr[19999]);
}

main();
