const { Readable } = require('node:stream');
const { EventEmitter } = require('node:events');

class Msg extends Readable {
  constructor(socket, parsed) {
    super();
    this.socket = socket;
    this.method = parsed.method;
    this.headers = parsed.rawHeaders || [];
    this._bodyConsumed = false;
  }
}

class Explicit extends EventEmitter {
  constructor(n) {
    super();
    this.n = n;
    if (n & 1) return; // implicit undefined return path
    return 42; // non-object return path: must still yield `this`
  }
}

let failures = 0;
function check(name, cond) {
  if (!cond) {
    failures++;
    console.log('FAIL', name);
  }
}

for (let i = 0; i < 300; i++) {
  const m = new Msg({ id: i }, { method: 'GET', rawHeaders: ['host', 'x'] });
  check('props survive at iter ' + i, m.method === 'GET' && m.headers.length === 2 && m._bodyConsumed === false);
  check('stream identity at iter ' + i, typeof m.pipe === 'function' && typeof m.unpipe === 'function');
  const e = new Explicit(i);
  check('explicit-return ctor yields this at iter ' + i, e.n === i && e instanceof EventEmitter);
  if (failures > 5) break;
}

console.log(failures === 0 ? 'ALL PASS' : failures + ' FAILURES');
if (failures) process.exitCode = 1;
