const p = Promise.resolve(123);

console.log(`t0.status:${p.status}`);
p.status = 'pending';
console.log(`t1.status:${p.status}`);
p.value = 123;
console.log(`t2.value:${p.value}`);

p.then((value) => {
  p.status = 'fulfilled';
  p.value = value;
  console.log(`then.status:${p.status}`);
  console.log(`then.value:${p.value}`);
});

Promise.resolve().then(() => {
  console.log(`micro.status:${p.status}`);
  console.log(`micro.value:${p.value}`);
});
