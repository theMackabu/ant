const readline = require('node:readline');

const rl = readline.createInterface({
  input: process.stdin,
  output: process.stdout,
});

rl.question('Q: ', answer => {
  console.log(`ANSWER ${JSON.stringify(answer)}`);
  rl.close();
});
