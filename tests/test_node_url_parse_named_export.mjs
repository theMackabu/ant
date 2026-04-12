import { parse } from 'node:url';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const tcp = parse('tcp://localhost:4321');
assert(tcp, 'expected parse() to return a value for tcp:// URLs');
assert(tcp.protocol === 'tcp:', `expected tcp protocol, got ${tcp && tcp.protocol}`);
assert(tcp.hostname === 'localhost', `expected localhost hostname, got ${tcp && tcp.hostname}`);
assert(tcp.port === '4321', `expected 4321 port, got ${tcp && tcp.port}`);

const unix = parse('unix:/tmp/ant-test.sock');
assert(unix, 'expected parse() to return a value for unix: URLs');
assert(unix.protocol === 'unix:', `expected unix protocol, got ${unix && unix.protocol}`);
assert(unix.pathname === '/tmp/ant-test.sock',
  `expected unix pathname, got ${unix && unix.pathname}`);

console.log('node:url parse named export test passed');
