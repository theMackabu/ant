const now =
  typeof performance !== 'undefined' && performance && typeof performance.now === 'function'
    ? () => performance.now()
    : () => Date.now();

function median(values) {
  const sorted = [...values].sort((a, b) => a - b);
  const mid = sorted.length >> 1;
  return sorted.length % 2 === 0
    ? (sorted[mid - 1] + sorted[mid]) / 2
    : sorted[mid];
}

function formatMetric(label, result) {
  console.log(
    `${label}: median=${result.medianMs.toFixed(3)}ms ` +
    `(${result.usPerOp.toFixed(3)}us/op, ${Math.round(result.opsPerSec)} ops/s)`
  );
}

function yieldTick() {
  return new Promise((resolve) => setTimeout(resolve, 0));
}

async function bench(label, iterations, fn, rounds = 5) {
  for (let i = 0; i < 2; i++) {
    await yieldTick();
    for (let j = 0; j < Math.min(1000, iterations); j++) {
      await fn();
    }
  }

  const samples = [];
  for (let round = 0; round < rounds; round++) {
    await yieldTick();
    const start = now();
    for (let i = 0; i < iterations; i++) {
      await fn();
    }
    samples.push(now() - start);
  }

  const medianMs = median(samples);
  const opsPerSec = iterations / (medianMs / 1000);
  const usPerOp = (medianMs * 1000) / iterations;
  const result = { medianMs, opsPerSec, usPerOp, samples };
  formatMetric(label, result);
  return result;
}

function getColorEnabled() {
  const { process, Deno } = globalThis;
  const isNoColor =
    typeof Deno?.noColor === 'boolean'
      ? Deno.noColor
      : process !== void 0
        ? 'NO_COLOR' in process?.env
        : false;
  return !isNoColor;
}

async function getColorEnabledAsync() {
  const { navigator } = globalThis;
  const cfWorkers = 'cloudflare:workers';
  const isNoColor =
    navigator !== void 0 && navigator.userAgent === 'Cloudflare-Workers'
      ? await (async () => {
          try {
            return 'NO_COLOR' in ((await import(cfWorkers)).env ?? {});
          } catch {
            return false;
          }
        })()
      : !getColorEnabled();
  return !isNoColor;
}

async function asyncWrapSync() {
  return !getColorEnabled();
}

async function navGuardOnly() {
  const { navigator } = globalThis;
  const isNoColor =
    navigator !== void 0 && navigator.userAgent === 'Cloudflare-Workers'
      ? false
      : !getColorEnabled();
  return !isNoColor;
}

async function deadAwaitBranch() {
  const { navigator } = globalThis;
  const isNoColor =
    navigator !== void 0 && navigator.userAgent === 'Cloudflare-Workers'
      ? await (async () => false)()
      : !getColorEnabled();
  return !isNoColor;
}

async function main() {
  const ua = globalThis.navigator?.userAgent ?? '';
  console.log(`navigator.userAgent=${ua || '<missing>'}`);

  if (!globalThis.navigator || ua === 'Cloudflare-Workers') {
    console.log('skipping dead-await fast-path regression outside the normal fast path');
    return;
  }

  const iterations = Number(process.env.ANT_ASYNC_DEAD_AWAIT_ITERS || 8000);
  const ratioLimit = Number(process.env.ANT_ASYNC_DEAD_AWAIT_RATIO_LIMIT || 6);

  const wrap = await bench('asyncWrapSync', iterations, asyncWrapSync);
  const guard = await bench('navGuardOnly', iterations, navGuardOnly);
  const dead = await bench('deadAwaitBranch', iterations, deadAwaitBranch);
  const real = await bench('getColorEnabledAsync', iterations, getColorEnabledAsync);

  const deadVsWrap = dead.usPerOp / wrap.usPerOp;
  const deadVsGuard = dead.usPerOp / guard.usPerOp;
  const realVsGuard = real.usPerOp / guard.usPerOp;

  console.log(`deadAwaitBranch/asyncWrapSync ratio=${deadVsWrap.toFixed(2)}x`);
  console.log(`deadAwaitBranch/navGuardOnly ratio=${deadVsGuard.toFixed(2)}x`);
  console.log(`getColorEnabledAsync/navGuardOnly ratio=${realVsGuard.toFixed(2)}x`);

  if (deadVsWrap > ratioLimit || deadVsGuard > ratioLimit) {
    throw new Error(
      'untaken await branch regressed the async fast path ' +
      `(ratios ${deadVsWrap.toFixed(2)}x/${deadVsGuard.toFixed(2)}x, limit ${ratioLimit}x)`
    );
  }
}

main().catch((error) => {
  console.error(error && error.stack ? error.stack : String(error));
  process.exitCode = 1;
});
