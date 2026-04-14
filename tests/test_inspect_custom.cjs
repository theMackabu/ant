const { inspect } = require('node:util');

function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

assert(typeof Symbol.inspect === 'symbol', 'expected Symbol.inspect to exist');
assert(Symbol.inspect !== Symbol.for('ant.inspect'), 'expected Symbol.inspect to stay separate from the global registry');
assert(Symbol.keyFor(Symbol.inspect) === undefined, 'expected Symbol.inspect to not be registry-backed');

class Connection {
  constructor(host, port, state) {
    this.host = host;
    this.port = port;
    this.state = state;
  }

  [Symbol.inspect]() {
    return `Connection { ${this.host}:${this.port} (${this.state}) }`;
  }
}

class FallbackConnection {
  constructor(host) {
    this.host = host;
  }

  [Symbol.inspect]() {
    throw new Error('boom');
  }
}

const blob = new Blob(['hi'], { type: 'text/plain' });
const file = new File(['hi'], 'note.txt', { type: 'text/plain', lastModified: 42 });
const timeout = setTimeout(() => {}, 1);
const interval = setInterval(() => {}, 5);
const timeoutInspect = inspect(timeout);
const intervalInspect = inspect(interval);

assert(inspect(new Connection('localhost', 3000, 'open')) === 'Connection { localhost:3000 (open) }', 'expected custom inspect result');
assert(inspect(blob) === "Blob { size: 2, type: 'text/plain' }", 'expected Blob custom inspect output');
assert(inspect(file) === "File { size: 2, type: 'text/plain', name: 'note.txt', lastModified: 42 }", 'expected File custom inspect output');
assert(
  timeoutInspect === 'Timeout (1) {\n  delay: 1,\n  repeat: null,\n  [Symbol(Symbol.toPrimitive)]: [native code]\n}',
  `expected legacy Timeout inspect output, got: ${timeoutInspect}`
);
assert(
  intervalInspect === 'Interval (2) {\n  delay: 5,\n  repeat: 5,\n  [Symbol(Symbol.toPrimitive)]: [native code]\n}',
  `expected legacy Interval inspect output, got: ${intervalInspect}`
);

clearTimeout(timeout);
clearInterval(interval);

const fallback = inspect(new FallbackConnection('db.internal'));
assert(
  fallback === "FallbackConnection { host: 'db.internal' }",
  `expected fallback formatting after inspect throw, got: ${fallback}`
);

console.log('PASS');
