import net from 'node:net';

function tryListen(port, host) {
  return new Promise((resolve) => {
    const server = net.createServer();
    server.once('error', (e) => {
      console.log(`  error on ${host}:${port} — code=${e.code} message=${e.message}`);
      server.close(() => resolve({ ok: false, code: e.code }));
    });
    server.once('listening', () => {
      const addr = server.address();
      console.log(`  listening on ${JSON.stringify(addr)}`);
      server.close(() => resolve({ ok: true }));
    });
    server.listen(port, host);
  });
}

const hosts = ['0.0.0.0', '::', '0000:0000:0000:0000:0000:0000:0000:0000'];
for (const host of hosts) {
  process.stdout.write(`tryListen(9874, '${host}') ... `);
  const result = await tryListen(9874, host);
  console.log(result.ok ? 'ok' : `fail(${result.code})`);
}

console.log('done');
