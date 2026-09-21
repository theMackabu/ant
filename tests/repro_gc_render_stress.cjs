// Dependency-free sustained rendering/GC stress.
console.log('repro: top level started');
const rounds = Number(process.argv[2] || 100000);
const dump = process.argv[3] === 'dump';
const retained = new Array(127);
const styles = new Map();
let iteration = 0;
let checksum = 0;

function makeStyle(code) {
  function style(text) {
    const pieces = String(text).split('\n');
    return pieces.map(part => '\x1b[' + code + 'm' + part + '\x1b[0m').join('\n');
  }
  style.code = code;
  style.parts = [code, '\x1b[0m'];
  return style;
}
for (let i = 0; i < 16; i++) styles.set(i, makeStyle(30 + i));
console.log('repro: styles initialized');

function span(text, style) {
  return { text, style };
}
function line(...segments) {
  return { type: 'styled', segments };
}
function prefix(value) {
  if (typeof value === 'string') return [span(value)];
  return value.map(segment => ({ text: segment.text, style: segment.style }));
}
function decorate(block, first, rest) {
  return block.map((entry, index) => {
    const lead = prefix(index === 0 ? first : rest);
    return line(...lead, ...entry.segments);
  });
}
function tokenize(text, style) {
  return text.split(/(\s+|[{}()[\],;])/).filter(Boolean).map((part, i) =>
    span(part, i % 3 === 0 ? style : undefined));
}
function render(text, width, style) {
  const wrapped = text.split('\n').flatMap(part => {
    const rows = [];
    for (let offset = 0; offset < part.length; offset += width) {
      rows.push(line(...tokenize(part.slice(offset, offset + width), style)));
    }
    return decorate(rows, [span('> ', style)], [span('  ')]);
  });
  return wrapped.map(entry => entry.segments.map(segment =>
    segment.style ? segment.style(segment.text) : segment.text).join(''));
}
function tick() {
  if (iteration === 0) console.log('repro: timer callback entered');
  const end = Math.min(iteration + 100, rounds);
  for (; iteration < end; iteration++) {
    const text = ('function example(x) { return [x, "value", { tag: true }]; }\n' +
      'Words **bold** and `code` with unicode → café █ 🐜\n').repeat(1 + iteration % 13);
    const style = styles.get(iteration % 16);
    const output = render(text, 20 + iteration % 100, style);
    const serialized = JSON.stringify({ iteration, output });
    if (dump) console.log('OUTPUT ' + serialized);
    const parsed = JSON.parse(serialized);
    if (parsed.iteration !== iteration || parsed.output.length !== output.length)
      throw new Error('corrupted output at ' + iteration);
    checksum += serialized.length;
    retained[iteration % retained.length] = {
      output, style, read: () => parsed.output[0], snapshot: parsed,
    };
  }
  if (iteration % 1000 === 0) console.log('round=' + iteration + ' checksum=' + checksum);
  if (iteration < rounds) setTimeout(tick, 0);
  else console.log('PASS: ' + rounds + ' rounds');
}
console.log('repro: scheduling timer');
setTimeout(() => {
  try {
    tick();
  } catch (error) {
    console.log('repro: callback threw: ' + (error && error.stack ? error.stack : error));
    process.exitCode = 1;
  }
}, 0);
