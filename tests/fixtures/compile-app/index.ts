import { greet, boom } from "./lib/util.ts";
import data from "./data.json";
import notes from "./notes.txt";
import { pad } from "left-tiny";
import { createRequire } from "node:module";

const cjs = await import("./lib/legacy.cjs");

console.log("greet:", greet("world"));
console.log("data:", data.a + data.b);
console.log("notes:", notes.trim());
console.log("pad:", pad("x", 4));
console.log("cjs:", cjs.default.tag);
console.log("argv:", JSON.stringify(process.argv.slice(2)));
console.log("execPath:", process.execPath);
console.log("dirname:", __dirname);

if (process.argv.includes("--throw")) boom();

if (process.argv.includes("--worker")) {
  const { Worker } = await import("node:worker_threads");
  const w = new Worker(new URL("./worker.ts", import.meta.url), { workerData: { n: 5 } });
  w.on("message", (m: unknown) => console.log("worker said:", m));
}

if (process.argv.includes("--fork")) {
  const { fork } = await import("node:child_process");
  const child = fork(new URL("./fork_child.mjs", import.meta.url).pathname);
  child.on("exit", (code: number) => console.log("fork exit:", code));
}
