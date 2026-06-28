// An escape sequence (arrow key) delivered through the shared stdin reader must
// decode as a single named keypress (e.g. "up"), not a spurious "escape" — the
// latter is what @clack/core maps to cancel, so a regression turns arrow keys
// into accidental cancellations.
const readline = require('node:readline');

readline.emitKeypressEvents(process.stdin);
if (process.stdin.isTTY) process.stdin.setRawMode(true);
process.stdin.resume();

const seen = [];
process.stdin.on('keypress', (str, key) => {
  seen.push(key && key.name);
  if (key && key.name === 'q') {
    if (process.stdin.isTTY) process.stdin.setRawMode(false);
    console.log(`KEYS=${JSON.stringify(seen)}`);
    process.exit(0);
  }
});

process.stdout.write('READY\n');
