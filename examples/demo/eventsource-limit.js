import http from 'node:http';

const server = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'text/event-stream' });

  if (req.url === '/ticks') {
    let n = 0;
    const timer = setInterval(() => res.write(`id: ${++n}\ndata: tick ${n}\n\n`), 100);
    req.on('close', () => clearInterval(timer));
    return;
  }

  const chunk = 'a'.repeat(64 * 1024);
  const timer = setInterval(() => res.write(chunk), 5);
  req.on('close', () => clearInterval(timer));
});

await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const base = `http://127.0.0.1:${server.address().port}`;

const ticks = await new Promise(resolve => {
  const seen = [];
  const source = new EventSource(`${base}/ticks`, { maxEventSize: 1024 });

  source.onmessage = event => {
    seen.push(event.data);
    if (seen.length < 3) return;
    source.close();
    resolve(seen);
  };
});

console.log('small events pass the limit:', ticks);

const rssBefore = process.memoryUsage().rss;
await new Promise(resolve => {
  const source = new EventSource(`${base}/runaway`, { maxEventSize: 1024 * 1024 });

  source.onmessage = () => console.log('unexpected message');
  source.onerror = () => {
    source.close();
    resolve();
  };
});

const grown = (process.memoryUsage().rss - rssBefore) / 1024 / 1024;
console.log(`runaway line stopped at 1 MiB, rss grew ${grown.toFixed(1)} MiB`);

try {
  new EventSource(`${base}/ticks`, { maxEventSize: 0 });
} catch (error) {
  console.log(`${error.name}: ${error.message}`);
}

server.close();
