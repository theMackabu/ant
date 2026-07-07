function readVoid(i) {
  const value = void 0;
  if (i > 100 && value !== undefined) {
    throw new Error(`void 0 became ${value}`);
  }
}

function readObjectVoid(i) {
  const object = { status: void 0 };
  if (i > 100 && object.status !== undefined) {
    throw new Error(`object status became ${object.status}`);
  }
}

for (let i = 1; i <= 105; i++) {
  readVoid(i);
  readObjectVoid(i);
}

console.log('jit void ok');
