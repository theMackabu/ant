const topDirname = import.meta.dirname;

function readNestedDirname() {
  return import.meta.dirname;
}

async function readAsyncDirname() {
  await 0;
  return import.meta.dirname;
}

export { topDirname };
export const nestedDirname = readNestedDirname();
export { readAsyncDirname };
