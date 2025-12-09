async function allocateAndWait() {
  let data = [];
  for (let i = 0; i < 100; i = i + 1) {
    data.push({ value: 'test ' + i });
  }
  console.log('Before await, data length:', data.length);

  await new Promise(resolve => setTimeout(resolve, 10));
  console.log(Ant.gc());

  console.log('After await+GC, data length:', data.length);
  return data.length;
}

async function main() {
  for (let i = 0; i < 3; i = i + 1) {
    console.log('Cycle', i + 1);
    let result = await allocateAndWait();
    console.log('Result:', result);
    Ant.gc();
  }
  console.log('Done');
}

main();
