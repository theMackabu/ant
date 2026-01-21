const funcs = [];
for (let i = 0; i < 3; i++) {
  funcs.push(() => i);
}
console.log(funcs[0](), funcs[1](), funcs[2]());
