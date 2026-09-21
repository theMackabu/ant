// Standalone sustained-allocation reproducer; no application dependencies.
// Run: ./build/ant tests/repro_gc_array_churn.cjs
const rounds = Number(process.argv[2] || 200000);
const retained = new Array(64);
let round = 0;
let checksum = 0;

function span(text, style) {
  return { text, style };
}

function line(...segments) {
  return { type: 'styled', segments };
}

function decorate(text, index) {
  const prefix = [span(' '), span(index % 2 ? '> ' : '- ')];
  const body = [span(text), span(' tail')];
  return line(...prefix, ...body);
}

function render(groups, iteration) {
  return groups.flatMap((group, groupIndex) =>
    group.map((text, index) => decorate(text + iteration, index + groupIndex)),
  );
}

function tick() {
  const end = Math.min(round + 1000, rounds);
  for (; round < end; round++) {
    const groups = [
      ['alpha', 'beta', 'gamma', 'delta'],
      ['one', 'two', 'three', 'four'],
      ['red', 'green', 'blue', 'white'],
      ['first', 'second', 'third', 'fourth'],
    ];
    const result = render(groups, round);
    if (result.length !== 16) throw new Error('corrupt result length: ' + result.length);
    for (let i = 0; i < result.length; i++) {
      const segments = result[i].segments;
      if (segments.length !== 4 || segments[3].text !== ' tail')
        throw new Error('corrupt segments at round ' + round + ', item ' + i);
      checksum += segments[2].text.length;
    }
    retained[round % retained.length] = result;
  }
  if (round % 10000 === 0) console.log('round=' + round + ' checksum=' + checksum);
  if (round < rounds) setTimeout(tick, 0);
  else console.log('PASS: ' + rounds + ' rounds');
}

setTimeout(tick, 0);
