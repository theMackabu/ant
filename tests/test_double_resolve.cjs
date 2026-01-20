(async () => {
  let p = Promise.resolve(1).then(x => x + 1);
  console.log(await p);
  console.log(await p);
})();
