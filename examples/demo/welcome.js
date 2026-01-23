console.log(`Hello from ${navigator.userAgent}! Did you know:\n`);

const rows = [
  ['Feature', 'Description'],
  ['Async/Await', 'Full coroutine support with minicoro'],
  ['HTTP Server', 'Built-in Ant.serve() with TLS support'],
  ['Fetch API', 'HTTP client with TLS via tlsuv'],
  ['File System', 'Async/sync fs with ant:fs module'],
  ['FFI', 'Native library integration'],
  ['Web Locks', 'Navigator.locks API'],
  ['TypeScript', 'Built-in type stripping via oxc'],
  ['Garbage Collection', 'Mark-copy compacting + Boehm-Demers'],
  null,
  [null, 'And more...']
];

const widths = [0, 0];
for (const r of rows)
  if (r) {
    if ((r[0]?.length || 0) > widths[0]) widths[0] = r[0].length;
    if ((r[1]?.length || 0) > widths[1]) widths[1] = r[1].length;
  }

const totalWidth = widths[0] + widths[1] + 3;
const border = (l, m, r) => l + '─'.repeat(widths[0] + 2) + m + '─'.repeat(widths[1] + 2) + r;

console.log(border('┌', '┬', '┐'));
let lastRowSpanned = false;

for (let i = 0; i < rows.length; i++) {
  const r = rows[i];
  lastRowSpanned = !r || !r[0] || !r[1];

  if (!r) console.log('│' + ' '.repeat(totalWidth + 2) + '│');
  else if (!r[0]) console.log('│ ' + r[1].padStart(totalWidth) + ' │');
  else if (!r[1]) console.log('│ ' + r[0].padEnd(totalWidth) + ' │');
  else console.log('│ ' + r[0].padEnd(widths[0]) + ' │ ' + r[1].padEnd(widths[1]) + ' │');

  if (i === 0) console.log(border('├', '┼', '┤'));
}
console.log(lastRowSpanned ? '└' + '─'.repeat(totalWidth + 2) + '┘' : border('└', '┴', '┘'));
