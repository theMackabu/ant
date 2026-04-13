const { Readable } = require('node:stream');

async function main() {
  const seen = [];

  const readable = new Readable({
    read() {
      this.push('x');
      this.push(null);
    },
  });

  readable.on('data', (chunk) => {
    seen.push(['data', chunk.toString('utf8')]);
  });

  readable.on('end', () => {
    seen.push(['end']);
  });

  await new Promise((resolve) => setTimeout(resolve, 0));

  const got = JSON.stringify(seen);
  const want = JSON.stringify([['data', 'x'], ['end']]);
  if (got !== want) {
    throw new Error(`expected ${want}, got ${got}`);
  }

  console.log('stream data listeners keep deferred flowing semantics');
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exit(1);
});
