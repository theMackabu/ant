async function main() {
  const value = await {
    then(resolve) {
      resolve(123);
    }
  };

  console.log(`await.thenable:${value}`);
}

await main();
