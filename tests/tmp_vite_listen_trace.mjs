import net from 'node:net';
import http from 'node:http';
import { promises as dns } from 'node:dns';

const wildcardHosts = new Set(['0.0.0.0', '::', '0000:0000:0000:0000:0000:0000:0000:0000']);

function tryListen(port, host) {
  return new Promise((resolve) => {
    const server = net.createServer();
    server.once('error', (e) => {
      server.close(() => resolve({ ok: false, code: e.code }));
    });
    server.once('listening', () => {
      server.close(() => resolve({ ok: true }));
    });
    server.listen(port, host);
  });
}

async function isPortAvailable(port) {
  for (const host of wildcardHosts) {
    console.log(`  isPortAvailable: tryListen(${port}, '${host}')`);
    const r = await tryListen(port, host).catch(() => ({ ok: true }));
    console.log(`  isPortAvailable: tryListen(${port}, '${host}') => ${JSON.stringify(r)}`);
    if (!r.ok) return false;
  }
  return true;
}

// step 1: dns.promises.lookup (resolveHostname)
console.log('step 1: dns.promises.lookup');
const [a, b] = await Promise.all([
  dns.lookup('localhost'),
  dns.lookup('localhost', { verbatim: true }),
]);
console.log('lookup results:', a, b);

// step 2: isPortAvailable
console.log('\nstep 2: isPortAvailable(5173)');
const avail = await isPortAvailable(5173);
console.log('isPortAvailable =>', avail);

// step 3: tryBindServer via real http.createServer + listen
console.log('\nstep 3: tryBindServer');
const httpServer = http.createServer((req, res) => res.end('ok'));

// mimic Vite's listen wrapper
const origListen = httpServer.listen.bind(httpServer);
httpServer.listen = async (port, ...args) => {
  console.log('  wrapped listen called, port=', port);
  return origListen(port, ...args);
};

const bindResult = await new Promise((resolve) => {
  const onError = (e) => {
    httpServer.off('error', onError);
    httpServer.off('listening', onListening);
    resolve({ success: false, error: e });
  };
  const onListening = () => {
    httpServer.off('error', onError);
    httpServer.off('listening', onListening);
    resolve({ success: true });
  };
  httpServer.on('error', onError);
  httpServer.on('listening', onListening);
  httpServer.listen(5173, 'localhost');
});

console.log('tryBindServer result:', bindResult.success ? 'success' : `fail: ${bindResult.error?.message}`);
if (bindResult.success) {
  console.log('address:', httpServer.address());
  await new Promise((resolve) => httpServer.close(resolve));
  console.log('server closed');
}

console.log('\ndone');
