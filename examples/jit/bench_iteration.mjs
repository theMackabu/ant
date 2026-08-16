const N = 6000;
const R = 300;
const NEIGHBOURS = 8;

const cells = [];
for (let i = 0; i < N; i++) cells.push({ alive: i % 3 === 0, neighbours: [] });
for (let i = 0; i < N; i++) {
  for (let k = 0; k < NEIGHBOURS; k++) cells[i].neighbours.push(cells[(i + k + 1) % N]);
}

const steps = N * R * NEIGHBOURS;

function best(fn) {
  let ms = Infinity;
  for (let r = 0; r < 5; r++) {
    const t0 = Date.now();
    const n = fn();
    const d = Date.now() - t0;
    if (!n) throw new Error('sink');
    if (d < ms) ms = d;
  }
  return ms;
}

function report(label, ms) {
  const ns = (ms * 1e6) / steps;
  console.log(label.padEnd(26) + String(ms + 'ms').padStart(8) + '  ' + ns.toFixed(1) + ' ns/step');
}

report(
  'for..of array',
  best(() => {
    let n = 0;
    for (let r = 0; r < R; r++) {
      for (const c of cells) {
        for (const q of c.neighbours) if (q.alive) n++;
      }
    }
    return n;
  })
);

report(
  'indexed for',
  best(() => {
    let n = 0;
    for (let r = 0; r < R; r++) {
      for (let i = 0; i < N; i++) {
        const a = cells[i].neighbours;
        for (let j = 0; j < a.length; j++) if (a[j].alive) n++;
      }
    }
    return n;
  })
);

report(
  'for..of, no prop read',
  best(() => {
    let n = 0;
    for (let r = 0; r < R; r++) {
      for (const c of cells) {
        for (const q of c.neighbours) n++;
      }
    }
    return n;
  })
);

const byKey = new Map();
for (let i = 0; i < N; i++) byKey.set('k' + i, cells[i]);
report(
  'for..of map.values()',
  best(() => {
    let n = 0;
    for (let r = 0; r < R * NEIGHBOURS; r++) {
      for (const c of byKey.values()) if (c.alive) n++;
    }
    return n;
  })
);
