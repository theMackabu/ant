function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const topFilename = import.meta.filename;

function nestedFilename() {
  return import.meta.filename;
}

async function asyncFilename() {
  await 0;
  return import.meta.filename;
}

const nested = nestedFilename();
const asyncValue = await asyncFilename();

assert(nested === topFilename, `nested import.meta.filename drifted: ${nested} !== ${topFilename}`);
assert(asyncValue === topFilename, `async import.meta.filename drifted: ${asyncValue} !== ${topFilename}`);

console.log(`import.meta.filename.capture:${topFilename}`);
