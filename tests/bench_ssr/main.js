// React server-side rendering through a request loop without I/O (see
// server.js). Usage: ant tests/bench_ssr/main.js [requests] [every]
//
// React loads its development build unless NODE_ENV is production, so it is
// set before React is imported; a static import would load React first.
process.env.NODE_ENV = 'production';

const requests = Number(process.argv[2] ?? 100000);
const every = Number(process.argv[3] ?? 10000);

const { default: run } = await import('./server.js');

const start = performance.now();
run(requests, every);
const ms = performance.now() - start;
console.log(`time: ${ms.toFixed(1)} ms, ${Math.round(requests / (ms / 1000))} requests/sec`);
