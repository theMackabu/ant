const uuid = '10000000-1000-4000-8000-100000000000'.replace(/[018]/g, c =>
  (+c ^ (Ant.Crypto.getRandomValues(new Uint8Array(1))[0] & (15 >> (+c / 4)))).toString(16)
);

console.log(uuid);
