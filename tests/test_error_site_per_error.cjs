const assert = require('node:assert');

// Each error reports its own location: user code that runs while another
// error is being raised (key coercion here) neither takes nor loses the site.
function siteLine(error) {
  const header = String(error.stack).replace(/\x1b\[[0-9;]*m/g, '').split('\n')[0];
  const match = /:(\d+):\d+$/.exec(header);
  assert.ok(match, `no location header in:\n${error.stack}`);
  return Number(match[1]);
}

let inner;
const key = {
  toString() {
    inner = new Error('inner');
    return 'p';
  },
};

let outer;
try { null[key]; } catch (e) { outer = e; }

assert.ok(inner instanceof Error);
assert.ok(outer instanceof TypeError);
assert.strictEqual(siteLine(inner), 15);
assert.strictEqual(siteLine(outer), 21);

let later;
try { undefined.x; } catch {}
later = new Error('later');
assert.strictEqual(siteLine(later), 30);

console.log('PASS error sites are per error');
