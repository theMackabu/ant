import * as readline from 'node:readline';

if (!process.stdin.isTTY) {
  console.log('Not a TTY, skipping interactive test');
  process.exit(0);
}

console.log('Keypress test - press keys to see parsed output');
console.log('Press Ctrl+C to exit\n');

readline.emitKeypressEvents(process.stdin);
process.stdin.setRawMode(true);
process.stdin.resume();

process.stdin.on('keypress', (str, key) => {
  console.log('keypress:', JSON.stringify({ str, key }));

  if (key.ctrl && key.name === 'c') {
    console.log('\nExiting...');
    process.stdin.setRawMode(false);
    process.stdin.pause();
    process.exit(0);
  }
});
