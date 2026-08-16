import { parentPort, workerData, isMainThread, getEnvironmentData } from 'node:worker_threads';

if (parentPort) {
  if (workerData && workerData.ropeStress) {
    let text = 'abcdefghijklm';
    for (let i = 0; i < workerData.ropeStress; i++) text += 'x';
    parentPort.postMessage({
      ok: text.length === workerData.ropeStress + 13,
      length: text.length,
      tail: text.slice(-4)
    });
  } else {
    parentPort.postMessage({
      ok: true,
      isMainThread,
      hasParentPort: parentPort !== null,
      workerData,
      environmentData: getEnvironmentData('shared-key')
    });
  }
  parentPort.unref();
}
