function fail(message) {
  console.log(`FAIL: ${message}`);
  process.exit(1);
}

if (typeof exports.__defineGetter__ !== 'function') {
  fail('CommonJS exports should inherit legacy accessors from Object.prototype');
}

if (typeof module.__defineGetter__ !== 'function') {
  fail('CommonJS module should inherit legacy accessors from Object.prototype');
}

exports.__defineGetter__('answer', function () {
  return 42;
});

if (exports.answer !== 42) {
  fail('getter installed on exports should resolve');
}

module.__defineGetter__('loadedFlag', function () {
  return typeof this.exports === 'object';
});

if (module.loadedFlag !== true) {
  fail('getter installed on module should resolve');
}

console.log('PASS');
