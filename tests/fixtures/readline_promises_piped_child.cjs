const readline = require('node:readline/promises');

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
});

async function main() {
  const input = await rl.question('Enter a number: ');
  console.log(`You entered: ${input}`);
  rl.close();
}

main();
