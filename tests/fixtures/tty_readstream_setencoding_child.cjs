const fs = require('fs');
const tty = require('tty');

const fd = fs.openSync('/dev/tty', 'r');
const stream = new tty.ReadStream(fd);

function cleanup() {
  try { stream.destroy(); } catch {}
}

stream.setEncoding('utf8');
process.stdout.write('READY\n');

const timeout = setTimeout(() => {
  cleanup();
  console.log('TIMEOUT');
  process.exit(3);
}, 2000);

stream.on('data', chunk => {
  clearTimeout(timeout);
  cleanup();

  if (typeof chunk !== 'string') {
    console.log('TYPE', typeof chunk);
    process.exit(4);
  }

  console.log('DATA', JSON.stringify(chunk));
  process.exit(chunk === 'A€\n' ? 0 : 5);
});

stream.resume();
