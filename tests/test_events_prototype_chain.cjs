const { EventEmitter } = require('events');

function fail(message) {
  console.log(`FAIL: ${message}`);
  process.exit(1);
}

if (Object.getPrototypeOf(EventEmitter.prototype) !== Object.prototype) {
  fail('EventEmitter.prototype should inherit from Object.prototype');
}

function Program() {}

if (typeof Program.prototype.__defineGetter__ !== 'function') {
  fail('fresh function prototypes should inherit legacy accessors');
}

Program.prototype.__proto__ = EventEmitter.prototype;

if (typeof Program.prototype.__defineGetter__ !== 'function') {
  fail('repointed prototypes should still inherit legacy accessors through EventEmitter.prototype');
}

Program.prototype.__defineGetter__('terminal', function () {
  return this._terminal;
});

const program = new Program();
program._terminal = 'xterm-256color';

if (program.terminal !== 'xterm-256color') {
  fail('getter installed after EventEmitter prototype reassignment should work');
}

if (typeof EventTarget === 'function' && Object.getPrototypeOf(EventTarget.prototype) !== Object.prototype) {
  fail('EventTarget.prototype should inherit from Object.prototype');
}

console.log('PASS');
