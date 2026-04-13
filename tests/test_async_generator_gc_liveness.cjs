function assert(cond, msg) {
  if (!cond) throw new Error(msg);
}

function churnGarbage(rounds, width) {
  const ring = new Array(8);
  for (let r = 0; r < rounds; r++) {
    const batch = new Array(width);
    for (let i = 0; i < width; i++) {
      batch[i] = {
        i,
        label: "item-" + i,
        arr: [i, i + 1, i + 2, i + 3],
      };
    }
    ring[r & 7] = batch;
  }
  return ring.length;
}

async function* delayedCounter() {
  await new Promise((resolve) => setTimeout(resolve, 0));
  yield 42;
}

async function runOnce() {
  let iter = delayedCounter();
  const nextPromise = iter.next();
  iter = null;

  churnGarbage(128, 256);

  const result = await nextPromise;
  assert(result && result.value === 42, "expected yielded value");
  assert(result.done === false, "expected unfinished iterator result");
}

async function main() {
  for (let i = 0; i < 64; i++) await runOnce();
  console.log("async generator survives GC after iterator becomes unreachable");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
