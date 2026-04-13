async function* delayedPair(tag) {
  await new Promise((resolve) => setTimeout(resolve, 0));
  yield tag + ":one";
  await new Promise((resolve) => setTimeout(resolve, 0));
  yield tag + ":two";
}

async function stressOverlap(iteration) {
  const it = delayedPair("overlap:" + iteration);
  const p1 = it.next();
  const p2 = it.next();
  const p3 = it.next();
  return Promise.allSettled([p1, p2, p3]);
}

async function stressReturn(iteration) {
  const it = delayedPair("return:" + iteration);
  const p1 = it.next();
  const p2 = it.return("done:" + iteration);
  return Promise.allSettled([p1, p2]);
}

async function stressThrow(iteration) {
  const it = delayedPair("throw:" + iteration);
  const p1 = it.next();
  const p2 = it.throw(new Error("boom:" + iteration));
  return Promise.allSettled([p1, p2]);
}

async function main() {
  for (let i = 0; i < 512; i++) {
    if ((i & 31) === 0) console.log("iter", i);
    await stressOverlap(i);
    await stressReturn(i);
    await stressThrow(i);
  }
  console.log("async generator crash stress completed");
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  throw err;
});
