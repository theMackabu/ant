const { openSync } = require('fs');
const tty = require('tty');

const fd = openSync('/dev/tty', 'r');
const stream = new tty.ReadStream(fd);
stream.setRawMode(true);

let settled = false;

stream.on('data', (data) => {
  if (settled) {
    return;
  }
  settled = true;
  try { stream.setRawMode(false); } catch {}
  console.log('DATA', JSON.stringify(Buffer.from(data).toString('latin1')));
  process.exit(0);
});

process.stdout.write('READY\n');
process.stdout.write('\x1b[c');

setTimeout(() => {
  if (settled) {
    return;
  }
  settled = true;
  try { stream.setRawMode(false); } catch {}
  console.log('TIMEOUT');
  process.exit(2);
}, 100);
