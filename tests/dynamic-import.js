async function main() {
  const module = await import('./export-test.js');
  module.hello('ant');
}

void main();
