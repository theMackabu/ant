const mode = process.argv[2] || "fetch";
const iterations = Number(process.argv[3] || 50_000);
const reportStats = process.argv.includes("--stats");
const NativeFunction = globalThis.Function;
const generated = [];

if (mode === "capture") {
  function CapturingFunction(...args) {
    generated.push(String(args[args.length - 1] || ""));
    return NativeFunction(...args);
  }

  Object.setPrototypeOf(CapturingFunction, NativeFunction);
  CapturingFunction.prototype = NativeFunction.prototype;
  globalThis.Function = CapturingFunction;
}

const { Elysia } = await import("elysia");
const routeResponse = mode === "fetch-response" || mode === "map-response"
  ? new Response("hello")
  : null;
const app = mode === "fetch-response"
  ? new Elysia().get("/", () => routeResponse).compile()
  : new Elysia().get("/", () => "hello").compile();
globalThis.Function = NativeFunction;

if (mode === "capture") {
  console.log("elysia1 generated functions: " + generated.length);
  for (let i = 0; i < generated.length; i++) {
    const source = generated[i];
    console.log("\n--- generated[" + i + "] ---\n" + source);
  }
} else if (mode === "fetch" || mode === "fetch-cached" || mode === "fetch-response") {
  const request = new Request("http://localhost/");
  const cachedFetch = mode === "fetch-cached" ? app.fetch : null;
  const invoke = cachedFetch ? () => cachedFetch(request) : () => app.fetch(request);

  for (let i = 0; i < 2_000; i++) await invoke();

  const start = performance.now();
  let response;
  for (let i = 0; i < iterations; i++) response = await invoke();
  const elapsed = performance.now() - start;

  if (!(response instanceof Response) || response.status !== 200) {
    throw new Error("unexpected app.fetch response");
  }
  if (await response.clone().text() !== "hello") {
    throw new Error("unexpected app.fetch body");
  }

  const label = mode === "fetch-cached"
    ? "cached fetch"
    : mode === "fetch-response" ? "cached Response route" : "app.fetch";
  console.log("elysia1 " + label + " no logger: " + elapsed.toFixed(2) + " ms");
} else if (mode === "map-response") {
  const { mapCompactResponse } = await import(
    "./node_modules/elysia/dist/adapter/web-standard/handler.js"
  );
  const request = new Request("http://localhost/");
  let response;

  for (let i = 0; i < 2_000; i++) response = mapCompactResponse(routeResponse, request);
  const start = performance.now();
  for (let i = 0; i < iterations; i++) response = mapCompactResponse(routeResponse, request);
  const elapsed = performance.now() - start;

  if (response !== routeResponse) throw new Error("mapCompactResponse changed Response identity");
  console.log("elysia1 map cached Response: " + elapsed.toFixed(2) + " ms");
} else if (mode === "raw") {
  const cached = new Response("hello");
  let sink;

  function bench(name, fn) {
    for (let i = 0; i < 2_000; i++) sink = fn();

    const start = performance.now();
    for (let i = 0; i < iterations; i++) sink = fn();
    const elapsed = performance.now() - start;

    if (!(sink instanceof Response)) throw new Error(name + " did not return Response");
    if (sink.status !== 200) throw new Error(name + " returned an unexpected status");
    console.log(name + ": " + elapsed.toFixed(2) + " ms");
  }

  bench("raw cached Response clone", () => cached.clone());
  bench("raw new Response no headers", () => new Response("hello"));
  bench(
    "raw new Response text header",
    () => new Response("hello", { headers: { "content-type": "text/plain" } }),
  );
} else {
  throw new Error("unknown mode: " + mode);
}

if (reportStats) {
  if (typeof Ant === "undefined") throw new Error("--stats requires Ant");
  console.log("ant allocation stats: " + JSON.stringify(Ant.stats().alloc));
}
