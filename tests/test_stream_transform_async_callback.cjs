const { Transform } = require('node:stream');

async function main() {
  let output = '';

  const tr = new Transform({
    transform(chunk, encoding, cb) {
      Promise.resolve().then(() => {
        cb(null, chunk.toString('utf8').toUpperCase());
      });
    },
  });

  tr.on('data', (chunk) => {
    output += chunk.toString('utf8');
  });

  const finished = new Promise((resolve, reject) => {
    tr.on('finish', resolve);
    tr.on('error', reject);
  });

  tr.write('abc');
  tr.end();
  await finished;

  if (output !== 'ABC') {
    throw new Error(`expected "ABC", got ${JSON.stringify(output)}`);
  }

  console.log('stream Transform async callback preserves receiver state');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
