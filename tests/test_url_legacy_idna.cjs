const url = require('url');

let failed = 0;
function check(name, actual, expected) {
  const ok = actual === expected;
  if (!ok) failed++;
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${ok ? '' : ` (got ${JSON.stringify(actual)}, want ${JSON.stringify(expected)})`}`);
}

let r = url.parse('http://münich.com/');
check('unicode hostname to punycode', r.hostname, 'xn--mnich-kva.com');
check('unicode host to punycode', r.host, 'xn--mnich-kva.com');
check('href uses punycode host', r.href, 'http://xn--mnich-kva.com/');

r = url.parse('http://日本.jp/path');
check('cjk hostname', r.hostname, 'xn--wgv71a.jp');
check('cjk href keeps path', r.href, 'http://xn--wgv71a.jp/path');

r = url.parse('http://MÜNICH.com');
check('idna lowercases non-ascii', r.hostname, 'xn--mnich-kva.com');

r = url.parse('http://exämple.com:8080/x?y#z');
check('port preserved after idna', r.host, 'xn--exmple-cua.com:8080');

r = url.parse('http://user:pä55@münich.com/');
check('auth untouched by idna', r.auth, 'user:pä55');
check('auth host converted', r.hostname, 'xn--mnich-kva.com');

r = url.parse('http://xn--mnich-kva.com/');
check('already-punycode passthrough', r.hostname, 'xn--mnich-kva.com');

r = url.parse('http://ＡＢ.com/');
check('fullwidth maps to ascii', r.hostname, 'ab.com');

let threw = null;
try {
  url.parse('http://a‌b.com/');
} catch (e) {
  threw = e;
}
check('invalid idna throws', threw && threw.message, 'Invalid URL');
check('invalid idna error code', threw && threw.code, 'ERR_INVALID_URL');

if (failed) {
  console.log(`${failed} checks failed`);
  process.exit(1);
}
console.log('OK');
