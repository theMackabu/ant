import assert from 'node:assert';
import url, { domainToASCII } from 'node:url';
import { domainToASCII as bareDomainToASCII } from 'url';

assert.strictEqual(typeof domainToASCII, 'function');
assert.strictEqual(domainToASCII, url.domainToASCII);
assert.strictEqual(domainToASCII, bareDomainToASCII);
assert.strictEqual(domainToASCII('münich.com'), 'xn--mnich-kva.com');

console.log('node:url domainToASCII export tests passed');
