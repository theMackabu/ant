import handle from './handler.js';

// A server's request loop without I/O: builds each request, calls the handler and
// checks the response. It prints "ready" once loaded, then "@<count> <body>" every
// `every` requests; the sampler measures the process on each of those lines.
const log = typeof console === 'object' ? console.log : print;

export default function run(requests, every) {
  log('ready');
  for (let i = 1; i <= requests; i++) {
    const response = handle({ method: 'GET', url: '/', headers: { host: '127.0.0.1' } });
    if (response.status !== 200) throw new Error('Unexpected status ' + response.status);
    if (i % every === 0) log('@' + i + ' ' + response.body);
  }
  log('done');
}
