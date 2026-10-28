const fs = require('fs');
const os = require('os');
const path = require('path');

function check(name, actual, expected) {
  if (!Object.is(actual, expected)) throw new Error(`${name}: expected ${expected}, got ${actual}`);
}

let seed = 7;
const rnd = (n) => {
  seed = (seed * 1103515245 + 12345) & 0x7fffffff;
  return seed % n;
};

const alphabet = [
  [0x61], [0x7a], [0x20], [0x31], [0xe9], [0xf1], [0x4e2d], [0x20ac],
  [0xd83d, 0xde00], [0xd835, 0xdcb3], [0xd800], [0xdc00], [0x78, 0xd83d],
];

function makeCase(count) {
  const units = [];
  for (let i = 0; i < count; i++) units.push(...alphabet[rnd(alphabet.length)]);
  let str = '';
  for (let i = 0; i < units.length; i += 512) str += String.fromCharCode(...units.slice(i, i + 512));
  return { str, units };
}

function unitsOf(s) {
  const out = [];
  for (let i = 0; i < s.length; i++) out.push(s.charCodeAt(i));
  return out.join();
}

const cases = [];
for (let i = 0; i < 14; i++) cases.push(makeCase(i % 3 === 0 ? 900 + rnd(900) : 5 + rnd(60)));

for (let iter = 0; iter < 6000; iter++) {
  const { str, units } = cases[rnd(cases.length)];
  const n = units.length;
  const i = rnd(n + 2), j = rnd(n + 2);
  switch (rnd(8)) {
    case 0:
      check('charCodeAt', str.charCodeAt(i), i < n ? units[i] : NaN);
      break;
    case 1: {
      const lo = Math.min(i, j, n), hi = Math.min(Math.max(i, j), n);
      check('slice', unitsOf(str.slice(lo, hi)), units.slice(lo, hi).join());
      break;
    }
    case 2:
      check('at', str.at(-1 - (i % n)), String.fromCharCode(units[n - 1 - (i % n)]));
      break;
    case 3: {
      let ok = true;
      for (let k = Math.min(i, n - 1); k >= 0 && k > i - 80; k--) if (str.charCodeAt(k) !== units[k]) ok = false;
      check('reverse walk', ok, true);
      break;
    }
    case 4: {
      const lo = Math.min(i, n);
      const needle = String.fromCharCode(...units.slice(lo, lo + 2));
      const expect = needle.length ? units.findIndex((_, k) => k >= Math.min(j, n) && units[k] === units[lo] && (needle.length < 2 || units[k + 1] === units[lo + 1])) : Math.min(j, n);
      check('indexOf', str.indexOf(needle, j), expect);
      break;
    }
    case 5: {
      const re = /[éñ中]/g;
      re.lastIndex = i;
      const m = re.exec(str);
      let expect = -1;
      for (let k = i; k < n; k++) if (units[k] === 0xe9 || units[k] === 0xf1 || units[k] === 0x4e2d) { expect = k; break; }
      check('regex index', m ? m.index : -1, expect);
      check('regex lastIndex', re.lastIndex, m ? expect + 1 : 0);
      break;
    }
    case 6: {
      const re = /(?<w>[a-z]+)(?<d>\d*)/dg;
      re.lastIndex = i;
      const m = re.exec(str);
      if (m) {
        check('indices start', m.indices[0][0], m.index);
        check('indices end', m.indices[0][1], m.index + m[0].length);
        check('indices group', m.indices.groups.w[0], m.index);
      }
      break;
    }
    case 7: {
      const garbage = [];
      for (let k = 0; k < 200; k++) garbage.push('g' + k + str.slice(k % 7, (k % 7) + 3));
      check('codePointAt', str.codePointAt(Math.min(i, n - 1)) >= 0, true);
      break;
    }
  }
}

const file = path.join(os.tmpdir(), `ant-utf16-pos-${process.pid}.txt`);
fs.writeFileSync(file, Buffer.from([0x61, 0xc3, 0x28, 0xe0, 0x41, 0xf0, 0x9f, 0x62, 0xc3, 0xa9, 0xf0, 0x9f, 0x98, 0x80, 0x80, 0x80, 0x63]));
const odd = fs.readFileSync(file, 'utf8');
fs.unlinkSync(file);

let mixed = '';
for (let i = 0; i < 30; i++) mixed += (i % 3 ? 'x' : 'é') + odd;
const forward = [];
for (let i = 0; i < mixed.length; i++) forward.push(mixed.charCodeAt(i));
for (let k = 0; k < 300; k++) {
  const re = /[éx]/g;
  re.lastIndex = (k * 37) % mixed.length;
  const m = re.exec(mixed);
  if (m) {
    check('consistent index', mixed[m.index], m[0]);
    check('consistent lastIndex', re.lastIndex, m.index + 1);
  }
  const back = (k * 53) % mixed.length;
  check('random access agrees', mixed.charCodeAt(back), forward[back]);
  check('reverse access agrees', mixed.charCodeAt(mixed.length - 1 - back), forward[mixed.length - 1 - back]);
}

console.log('ok');
