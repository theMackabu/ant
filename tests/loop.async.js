let count = 0;

async function meow() {
  count++;
  return 'hi';
}

async function server() {
  return await meow();
}

async function main() {
  await server();
}

for (let i = 0; i < 40000; i++) {
  void main();
}

console.log('done', count);
