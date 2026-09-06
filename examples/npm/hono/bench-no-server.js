import { Hono } from 'hono';

const iterations = Number(process.argv[2] || 50_000);
const reportStats = process.argv.includes('--stats');
const app = new Hono();
app.get('/', context => context.text('ant'));

const request = new Request('http://localhost/');
let response;

for (let i = 0; i < 2_000; i++) response = await app.fetch(request);

const start = performance.now();
for (let i = 0; i < iterations; i++) response = await app.fetch(request);
const elapsed = performance.now() - start;

if (!(response instanceof Response) || response.status !== 200) {
  throw new Error('unexpected app.fetch response');
}
if (await response.clone().text() !== 'ant') {
  throw new Error('unexpected app.fetch body');
}

console.log(`hono app.fetch: ${elapsed.toFixed(2)} ms`);
if (reportStats) {
  if (typeof Ant === 'undefined') throw new Error('--stats requires Ant');
  console.log(`ant allocation stats: ${JSON.stringify(Ant.stats().alloc)}`);
}
