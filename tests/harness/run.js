import { run } from './harness.js';
import { targets } from './manifest.js';

const args = process.argv.slice(2);
const groups = [];
let only = null;
let list = false;
let update = false;

for (let i = 0; i < args.length; i++) {
  const a = args[i];
  if (a === '--list') list = true;
  else if (a === '--update') update = true;
  else if (a === '--only') only = args[++i];
  else if (a.startsWith('--only=')) only = a.slice(7);
  else groups.push(a);
}

const allTargets = targets();
let selected = allTargets;
if (groups.length) selected = selected.filter(t => groups.includes(t.group));
else selected = selected.filter(t => !t.defaultOff);
if (only) selected = selected.filter(t => t.name.includes(only));

if (!selected.length) {
  const availableGroups = [...new Set(allTargets.map(target => target.group))];
  console.log(`no targets match; groups are: ${availableGroups.join(', ')}`);
  process.exit(2);
}

if (list) {
  for (const t of selected) console.log(`${t.group.padEnd(8)} ${t.type.padEnd(7)} ${t.name}`);
  process.exit(0);
}

const ok = await run(selected, { update });
process.exit(ok ? 0 : 1);
