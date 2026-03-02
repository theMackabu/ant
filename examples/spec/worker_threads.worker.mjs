import { parentPort, workerData, isMainThread, getEnvironmentData } from 'node:worker_threads';

if (parentPort) {
  parentPort.postMessage({
    ok: true,
    isMainThread,
    hasParentPort: parentPort !== null,
    workerData,
    environmentData: getEnvironmentData('shared-key')
  });
  parentPort.unref();
}
