import { spawn } from 'child_process';

function sleep(ms) {
  return new Promise(resolve => setTimeout(resolve, ms));
}

async function waitForHttpServer(port, { host = '127.0.0.1', timeoutMs = 1000, intervalMs = 5 } = {}) {
  const deadline = performance.now() + timeoutMs;
  let lastError = null;

  while (performance.now() < deadline) {
    try {
      const response = await fetch(`http://${host}:${port}/__ant_spec_ready__`);
      await response.text();
      return;
    } catch (error) {
      lastError = error;
      await sleep(intervalMs);
    }
  }

  const detail = lastError && lastError.message ? `: ${lastError.message}` : '';
  throw new Error(`Timed out waiting for fixture server on ${host}:${port}${detail}`);
}

export async function startServer(path, port, options) {
  const child = spawn(process.execPath, [path, String(port)]);
  child.on('stderr', data => {
    if (String(data).trim()) console.log(String(data).trim());
  });

  try {
    await waitForHttpServer(port, options);
    return child;
  } catch (error) {
    child.kill('SIGTERM');
    throw error;
  }
}
