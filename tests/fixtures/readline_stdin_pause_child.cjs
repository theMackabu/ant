// process.stdin.pause() must stop ALL delivery while a readline Interface keeps
// the shared reader registered: no "data" events, and no readline "line" events
// or echo (Node stops the Interface on pause too).
const readline = require('node:readline');

const rl = readline.createInterface({ input: process.stdin, output: process.stdout });
rl.on('line', l => console.log(`LINE:${JSON.stringify(l)}`));
process.stdin.on('data', d => console.log(`DATA:${JSON.stringify(d.toString())}`));

process.stdin.pause();
console.log('READY');

setTimeout(() => {
  console.log('DONE');
  process.exit(0);
}, 1500);
