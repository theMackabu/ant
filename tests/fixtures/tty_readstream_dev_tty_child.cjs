const fs = require('fs');
const tty = require('tty');

const fd = fs.openSync('/dev/tty', 'r');
const stream = new tty.ReadStream(fd);

console.log('READY');

const timeout = setTimeout(() => {
  console.log('TIMEOUT');
  process.exit(3);
}, 2000);

stream.on('data', chunk => {
  clearTimeout(timeout);
  console.log('DATA', JSON.stringify(Buffer.from(chunk).toString('utf8')));
  process.exit(0);
});

stream.resume();
