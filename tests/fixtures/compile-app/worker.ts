import { parentPort, workerData } from "node:worker_threads";

const n: number = workerData?.n ?? 0;
parentPort?.postMessage(`worker-ok:${n}`);
