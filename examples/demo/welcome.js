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
  ['', ''],
  ['And more...']
];

const widths = [0, 0];
for (const r of rows) {
  if (r[0].length > widths[0]) widths[0] = r[0].length;
  if (r[1].length > widths[1]) widths[1] = r[1].length;
}

const border = '+' + '-'.repeat(widths[0] + 2) + '+' + '-'.repeat(widths[1] + 2) + '+';
console.log(border);
for (let i = 0; i < rows.length; i++) {
  const r = rows[i];
  console.log('| ' + r[0].padEnd(widths[0], ' ') + ' | ' + r[1].padEnd(widths[1], ' ') + ' |');
  if (i === 0) console.log(border);
}
console.log(border);
