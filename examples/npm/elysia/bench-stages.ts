const iterations = Number(process.argv[2] || 50_000);
const selected = process.argv[3];

const { Elysia } = await import("elysia");
const { createContext } = await import("./node_modules/elysia/dist/context.js");
const { mapCompactResponse } = await import(
  "./node_modules/elysia/dist/adapter/web-standard/handler.js"
);

const app = new Elysia();
const Context = createContext(app);
const request = new Request("http://localhost/");
const pathStart = "http://localhost".length;
const handler = () => "hello";
const inline = (context) => mapCompactResponse(handler(context), context.request);
let sink;

function makeContext() {
  return new Context(request);
}

function parsePath(context) {
  const url = request.url;
  const start = url.indexOf("/", pathStart);
  return context.path = url.substring(
    start,
    (context.qi = url.indexOf("?", start)) === -1 ? url.length : context.qi,
  );
}

function responseTag(response) {
  if (response === null || response === undefined) return undefined;
  const proto = Object.getPrototypeOf(response);
  if (proto === null) return "Object";
  return proto.constructor?.name;
}

function bench(name, fn) {
  if (selected && selected !== name) return;
  for (let i = 0; i < 2_000; i++) sink = fn();
  const start = performance.now();
  for (let i = 0; i < iterations; i++) sink = fn();
  const elapsed = performance.now() - start;
  console.log(name + ": " + elapsed.toFixed(2) + " ms");
}

const reusableContext = makeContext();
bench("context construction", makeContext);
bench("request URL path parse", () => parsePath(reusableContext));
bench("response tag", () => responseTag("hello"));
bench("compact string response", () => mapCompactResponse("hello", request));
bench("inline handler, reused context", () => inline(reusableContext));
bench("context + path + inline handler", () => {
  const context = makeContext();
  parsePath(context);
  return inline(context);
});

if (sink === undefined) throw new Error("benchmark result was not observed");
