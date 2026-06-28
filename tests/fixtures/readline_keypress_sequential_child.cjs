// Mirrors @clack/core's prompt shape: a readline Interface created WITHOUT an
// `output` (so it must not echo) used purely to track rl.line, while the value
// is read from process.stdin "keypress" events. Two prompts run sequentially so
// the second must re-subscribe a "keypress" listener after the first removed its
// own mid-keypress (the deferred-free regression).
const readline = require('node:readline');

function ask(tag) {
  return new Promise(resolve => {
    const rl = readline.createInterface({ input: process.stdin, terminal: true });
    if (process.stdin.isTTY) process.stdin.setRawMode(true);

    let value = '';
    const onKey = (str, key) => {
      if (key && key.name === 'return') {
        process.stdin.removeListener('keypress', onKey);
        if (process.stdin.isTTY) process.stdin.setRawMode(false);
        rl.close();
        resolve(value);
        return;
      }
      // Capture incrementally like @clack/core: rl.line is already updated by the
      // shared reader before this keypress fires.
      value = rl.line;
    };

    process.stdin.on('keypress', onKey);
    process.stdout.write(`READY:${tag}\n`);
  });
}

(async () => {
  const a = await ask('one');
  console.log(`A=${JSON.stringify(a)}`);
  const b = await ask('two');
  console.log(`B=${JSON.stringify(b)}`);
  process.exit(0);
})();
