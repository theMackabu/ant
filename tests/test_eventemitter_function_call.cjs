const { EventEmitter } = require('events');

function fail(message) {
  console.log(`FAIL: ${message}`);
  process.exit(1);
}

function Program() {
  EventEmitter.call(this);
}

Program.prototype = {};
Program.prototype.__proto__ = EventEmitter.prototype;
Program.prototype.constructor = Program;

const program = new Program();

if (!(program instanceof Program)) {
  fail('program should be an instance of Program');
}

let seen = 0;
program.on('tick', () => {
  seen++;
});
program.emit('tick');

if (seen !== 1) {
  fail('EventEmitter.call(this) should initialize event methods on custom receivers');
}

const ee = EventEmitter.call({});
if (typeof ee !== 'object') {
  fail('EventEmitter.call({}) should return the receiver object');
}

console.log('PASS');
