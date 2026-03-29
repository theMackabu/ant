const TOTAL = Number(process.env.TOTAL || 5000000);
const MODE = process.env.MODE || "routeSync";

function nowMs() {
  return performance.now();
}

function makeContext() {
  return {
    finalized: false,
    req: {
      routeIndex: -1
    },
    res: null,
    text(body, status = 200) {
      this.finalized = true;
      this.res = { body, status };
      return this.res;
    }
  };
}

function routeSync(c) {
  return c.text("ok");
}

function runTightLoop() {
  let acc = 0;
  for (let i = 0; i < TOTAL; i++) {
    acc += i;
  }
  return acc;
}

function runRouteSyncLoop() {
  let finalized = 0;
  for (let i = 0; i < TOTAL; i++) {
    const ctx = makeContext();
    routeSync(ctx);
    if (ctx.finalized) finalized++;
  }
  return finalized;
}

function main() {
  const startedAt = nowMs();
  const result = MODE === "tightLoop" ? runTightLoop() : runRouteSyncLoop();
  const elapsedMs = nowMs() - startedAt;

  console.log(`mode=${MODE}`);
  console.log(`total=${TOTAL}`);
  console.log(`elapsed_ms=${elapsedMs.toFixed(3)}`);
  console.log(`ops_per_sec=${(TOTAL / (elapsedMs / 1000)).toFixed(1)}`);
  console.log(`result=${result}`);
}

main();
