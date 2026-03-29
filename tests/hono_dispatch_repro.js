const TOTAL = Number(process.env.TOTAL || 20000);
const CONCURRENCY = Number(process.env.CONCURRENCY || 64);
const MODE = process.env.MODE || "composeAwaitNext";

function nowMs() {
  return performance.now();
}

function percentile(sorted, p) {
  if (sorted.length === 0) return 0;
  const index = Math.min(sorted.length - 1, Math.floor(sorted.length * p));
  return sorted[index];
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

function composeLike(middleware, onError, onNotFound) {
  return (context, next) => {
    let index = -1;
    return dispatch(0);

    async function dispatch(i) {
      if (i <= index) {
        throw new Error("next() called multiple times");
      }
      index = i;

      let res;
      let isError = false;
      let handler;

      if (middleware[i]) {
        handler = middleware[i];
        context.req.routeIndex = i;
      } else {
        handler = i === middleware.length && next || void 0;
      }

      if (handler) {
        try {
          res = await handler(context, () => dispatch(i + 1));
        } catch (err) {
          if (err instanceof Error && onError) {
            res = await onError(err, context);
            isError = true;
          } else {
            throw err;
          }
        }
      } else if (context.finalized === false && onNotFound) {
        res = await onNotFound(context);
      }

      if (res && (context.finalized === false || isError)) {
        context.res = res;
      }
      return context;
    }
  };
}

function routeSync(c) {
  return c.text("ok");
}

function routeResolved(c) {
  return Promise.resolve(c.text("ok"));
}

function routeAsync(c) {
  return (async () => c.text("ok"))();
}

function middlewareReturnNext(_c, next) {
  return next();
}

async function middlewareAwaitNext(_c, next) {
  return await next();
}

async function middlewareAwaitResolvedAround(_c, next) {
  await Promise.resolve();
  const out = await next();
  await Promise.resolve();
  return out;
}

function makeRunner(mode) {
  switch (mode) {
    case "directSync":
      return async () => routeSync(makeContext());
    case "directResolved":
      return async () => routeResolved(makeContext());
    case "directAsync":
      return async () => routeAsync(makeContext());
    case "composeRouteSync": {
      const app = composeLike([routeSync]);
      return async () => app(makeContext());
    }
    case "composeRouteResolved": {
      const app = composeLike([routeResolved]);
      return async () => app(makeContext());
    }
    case "composeRouteAsync": {
      const app = composeLike([routeAsync]);
      return async () => app(makeContext());
    }
    case "composeReturnNext": {
      const app = composeLike([middlewareReturnNext, routeSync]);
      return async () => app(makeContext());
    }
    case "composeAwaitNext": {
      const app = composeLike([middlewareAwaitNext, routeSync]);
      return async () => app(makeContext());
    }
    case "composeAwaitResolvedAround": {
      const app = composeLike([middlewareAwaitResolvedAround, routeSync]);
      return async () => app(makeContext());
    }
    case "composeReturnNextResolvedRoute": {
      const app = composeLike([middlewareReturnNext, routeResolved]);
      return async () => app(makeContext());
    }
    case "composeAwaitNextResolvedRoute": {
      const app = composeLike([middlewareAwaitNext, routeResolved]);
      return async () => app(makeContext());
    }
    case "composeReturnNextAsyncRoute": {
      const app = composeLike([middlewareReturnNext, routeAsync]);
      return async () => app(makeContext());
    }
    case "composeAwaitNextAsyncRoute": {
      const app = composeLike([middlewareAwaitNext, routeAsync]);
      return async () => app(makeContext());
    }
    default:
      throw new Error(`Unknown MODE: ${mode}`);
  }
}

async function main() {
  const runOne = makeRunner(MODE);
  const latencies = new Array(TOTAL);
  let nextIndex = 0;
  let completed = 0;
  const startedAt = nowMs();

  async function worker() {
    for (;;) {
      const index = nextIndex++;
      if (index >= TOTAL) return;

      const opStart = nowMs();
      await runOne();
      latencies[index] = nowMs() - opStart;
      completed++;
    }
  }

  const workers = [];
  for (let i = 0; i < CONCURRENCY; i++) {
    workers.push(worker());
  }
  await Promise.all(workers);

  const elapsedMs = nowMs() - startedAt;
  latencies.sort((a, b) => a - b);

  console.log(`mode=${MODE}`);
  console.log(`total=${TOTAL}`);
  console.log(`concurrency=${CONCURRENCY}`);
  console.log(`elapsed_ms=${elapsedMs.toFixed(3)}`);
  console.log(`avg_ms=${(elapsedMs / TOTAL).toFixed(3)}`);
  console.log(`rps=${(TOTAL / (elapsedMs / 1000)).toFixed(1)}`);
  console.log(`p50_ms=${percentile(latencies, 0.50).toFixed(3)}`);
  console.log(`p95_ms=${percentile(latencies, 0.95).toFixed(3)}`);
  console.log(`p99_ms=${percentile(latencies, 0.99).toFixed(3)}`);
  console.log(`completed=${completed}`);
}

main().catch((err) => {
  console.error(err && err.stack ? err.stack : String(err));
  process.exitCode = 1;
});
