const now = () => (typeof performance !== 'undefined' ? performance.now() : Date.now());

function makeChildHeavyFunction(childCount) {
  let src = 'return function run(seed) {\n';
  for (let i = 0; i < childCount; i++) {
    src += `function child_${i}(x) { return x + ${i}; }\n`;
  }
  src += 'let acc = seed;\n';
  for (let i = 0; i < childCount; i++) {
    src += `acc = child_${i}(acc);\n`;
  }
  src += 'return acc;\n';
  src += '}';
  return new Function(src)();
}

function makeTemplateHeavyFunction(siteCount) {
  let src = 'function tag(strs, ...args) { return strs.length + args.length; }\n';
  src += 'return function run(seed) {\n';
  src += 'let acc = 0;\n';
  for (let i = 0; i < siteCount; i++) {
    src += `acc += tag\`${i}:\${seed}:${i}\`;\n`;
  }
  src += 'return acc;\n';
  src += '}';
  return new Function(src)();
}

function churnGarbage(rounds, width) {
  const ring = new Array(8);
  for (let r = 0; r < rounds; r++) {
    const batch = new Array(width);
    for (let i = 0; i < width; i++) {
      batch[i] = {
        i,
        text: 'x' + i,
        arr: [i, i + 1, i + 2, i + 3]
      };
    }
    ring[r & 7] = batch;
  }
  return ring.length;
}

function snapshot() {
  return Ant.raw.gcMarkProfile();
}

function diff(after, before) {
  return {
    collections: after.collections - before.collections,
    funcVisits: after.funcVisits - before.funcVisits,
    childEdges: after.childEdges - before.childEdges,
    constSlots: after.constSlots - before.constSlots,
    timeMs: after.timeMs - before.timeMs
  };
}

function memorySnapshot() {
  const s = Ant.stats();
  const pools = s.pools?.totalUsed ?? 0;
  const alloc = s.alloc?.total ?? s.arenaUsed ?? 0;
  return {
    logical: pools + alloc,
    pools,
    alloc,
    rss: s.rss ?? 0,
    resident: s.residentSize ?? 0,
    footprint: s.physFootprint ?? 0
  };
}

function maxMemory(a, b) {
  return {
    logical: Math.max(a.logical, b.logical),
    pools: Math.max(a.pools, b.pools),
    alloc: Math.max(a.alloc, b.alloc),
    rss: Math.max(a.rss, b.rss),
    resident: Math.max(a.resident, b.resident),
    footprint: Math.max(a.footprint, b.footprint)
  };
}

function fmtMB(bytes) {
  return (bytes / 1024 / 1024).toFixed(1) + 'MB';
}

function runCase(name, factory, opts = {}) {
  const fnCount = opts.fnCount ?? 24;
  const rounds = opts.rounds ?? 24;
  const garbageRounds = opts.garbageRounds ?? 64;
  const garbageWidth = opts.garbageWidth ?? 192;

  const fns = [];
  for (let i = 0; i < fnCount; i++) fns.push(factory());

  Ant.raw.gcMarkProfileReset();
  const before = snapshot();
  const memBefore = memorySnapshot();
  let memPeak = memBefore;

  let checksum = 0;
  const start = now();
  for (let r = 0; r < rounds; r++) {
    for (let i = 0; i < fns.length; i++) checksum += fns[i](r);
    churnGarbage(garbageRounds, garbageWidth);
    memPeak = maxMemory(memPeak, memorySnapshot());
  }
  const elapsed = now() - start;

  const after = snapshot();
  const stats = diff(after, before);
  const memAfter = memorySnapshot();
  memPeak = maxMemory(memPeak, memAfter);

  console.log(`\n[${name}]`);
  console.log(`elapsed_ms=${elapsed.toFixed(2)}`);
  console.log(`collections=${stats.collections}`);
  console.log(`func_visits=${stats.funcVisits}`);
  console.log(`child_edges=${stats.childEdges}`);
  console.log(`const_slots=${stats.constSlots}`);
  console.log(`mark_func_ms=${stats.timeMs.toFixed(3)}`);
  console.log(
    `memory_before=${fmtMB(memBefore.logical)} memory_after=${fmtMB(memAfter.logical)} memory_peak=${fmtMB(memPeak.logical)}`
  );
  console.log(
    `memory_parts_after=pools:${fmtMB(memAfter.pools)} alloc:${fmtMB(memAfter.alloc)} rss:${fmtMB(memAfter.rss)} resident:${fmtMB(memAfter.resident)} footprint:${fmtMB(memAfter.footprint)}`
  );
  console.log(`checksum=${checksum}`);

  if (opts.expectedChecksum !== undefined && checksum !== opts.expectedChecksum) {
    throw new Error(
      `${name} checksum mismatch: got ${checksum}, expected ${opts.expectedChecksum}`
    );
  }
}

Ant.raw.gcMarkProfileEnable(true);
Ant.raw.gcMarkProfileReset();

runCase('child-func-heavy', () => makeChildHeavyFunction(160), {
  fnCount: 20,
  rounds: 22,
  garbageRounds: 72,
  garbageWidth: 224,
  expectedChecksum: 5601420
});

runCase('template-slot-heavy', () => makeTemplateHeavyFunction(160), {
  fnCount: 20,
  rounds: 22,
  garbageRounds: 72,
  garbageWidth: 224,
  expectedChecksum: 211200
});

runCase(
  'mixed',
  () => {
    const childHeavy = makeChildHeavyFunction(96);
    const templateHeavy = makeTemplateHeavyFunction(96);
    return function run(seed) {
      return childHeavy(seed) + templateHeavy(seed);
    };
  },
  {
    fnCount: 20,
    rounds: 22,
    garbageRounds: 72,
    garbageWidth: 224,
    expectedChecksum: 2137740
  }
);
