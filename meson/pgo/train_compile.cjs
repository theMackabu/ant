const fs = require('node:fs');
const path = require('node:path');

const root = path.join(__dirname, '..', '..');

const sources = [
  { dir: 'src/builtins', every: 1 },
  { dir: 'src/pkg', every: 1 },
  { dir: 'examples', every: 1 },
  { dir: 'packages', every: 1 },
  { dir: 'tools', every: 1 },
  { dir: 'tests/harness', every: 1 },
  { dir: '.github/agents', every: 1 },
  { dir: 'tests', every: 10 },
];

const skipped = ['examples/bench-v8', 'examples/spec', 'examples/test262', 'tests/harness', 'tests/fixtures'];
const rounds = +(process.env.ROUNDS || 8);

function walk(dir, top, out) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1))) {
    const full = path.join(dir, entry.name);
    const rel = path.relative(root, full);
    if (entry.name === 'node_modules' || entry.name.startsWith('.git')) continue;
    if (rel !== top && skipped.includes(rel)) continue;
    if (entry.isDirectory()) walk(full, top, out);
    else if (/\.(js|mjs|cjs|ts|mts)$/.test(entry.name) && !entry.name.endsWith('.d.ts')) out.push(full);
  }
  return out;
}

const files = [];
for (const { dir, every } of sources) {
  const full = path.join(root, dir);
  if (!fs.existsSync(full)) continue;
  files.push(...walk(full, dir, []).filter((_, index) => index % every === 0));
}

const units = [];
let bytes = 0;
for (const file of files) {
  const source = fs.readFileSync(file, 'utf8');
  try {
    Ant.raw.compileOnly(source, file);
    units.push({ source, file });
    bytes += source.length;
  } catch {}
}

if (units.length < files.length * 0.9) throw new Error(`only ${units.length} of ${files.length} sources compile`);

for (let round = 1; round < rounds; round++)
  for (const { source, file } of units) Ant.raw.compileOnly(source, file);

console.log(`${units.length}/${files.length} sources, ${bytes} bytes, ${rounds} rounds`);
