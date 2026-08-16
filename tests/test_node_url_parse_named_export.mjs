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
assert(unix.host === '', `expected empty unix host, got ${unix && unix.host}`);
assert(unix.hostname === '', `expected empty unix hostname, got ${unix && unix.hostname}`);
assert(unix.pathname === '/tmp/ant-test.sock',
  `expected unix pathname, got ${unix && unix.pathname}`);

const requestTarget = parse('/docs/index.html?theme=dark#contents');
assert(requestTarget, 'expected parse() to return a value for relative request targets');
assert(requestTarget.protocol === null,
  `expected a null relative protocol, got ${requestTarget.protocol}`);
assert(requestTarget.pathname === '/docs/index.html',
  `expected relative pathname, got ${requestTarget.pathname}`);
assert(requestTarget.search === '?theme=dark',
  `expected relative search, got ${requestTarget.search}`);
assert(requestTarget.query === 'theme=dark',
  `expected relative query, got ${requestTarget.query}`);
assert(requestTarget.hash === '#contents',
  `expected relative hash, got ${requestTarget.hash}`);
assert(requestTarget.path === '/docs/index.html?theme=dark',
  `expected relative path, got ${requestTarget.path}`);
assert(requestTarget.href === '/docs/index.html?theme=dark#contents',
  `expected relative href, got ${requestTarget.href}`);

console.log('node:url parse named export test passed');
