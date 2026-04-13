const { openSync } = require('fs');
const tty = require('tty');

const QUERY_KITTY = '\x1b[?u';
const QUERY_DA1 = '\x1b[c';
const ENABLE_FOCUS = '\x1b[?1004h';
const ENABLE_INBAND_RESIZE = '\x1b[?2048h';
const ENABLE_BRACKETED_PASTE = '\x1b[?2004h';
const ENABLE_KITTY_KEYBOARD = '\x1b[>7u';

const startedAt = Date.now();

function nowMs() {
  return Date.now() - startedAt;
}

function writeLog(record) {
  process.stderr.write(`${JSON.stringify({ t: nowMs(), ...record })}\n`);
}

function toHex(data) {
  return Buffer.from(data).toString('hex');
}

function cleanup(stream) {
  if (!stream) {
    return;
  }
  try {
    stream.setRawMode(false);
  } catch {}
  try {
    stream.removeAllListeners('data');
  } catch {}
  try {
    stream.destroy();
  } catch {}
}

function parseDeviceAttributes(data) {
  const text = Buffer.from(data).toString('latin1');
  const match = text.match(/\x1b\[\?(\d+)((?:;\d+)*)c/);
  if (!match) {
    return null;
  }

  const primary = Number(match[1]);
  const secondary = match[2]
    ? match[2].split(';').filter(Boolean).map((value) => Number(value))
    : [];

  return { primary, secondary };
}

async function main() {
  const fd = openSync('/dev/tty', 'r');
  const stream = new tty.ReadStream(fd);
  stream.on('error', (error) => {
    writeLog({ type: 'stream-error', message: error && error.message ? error.message : String(error) });
    process.exitCode = 1;
  });
  stream.setRawMode(true);

  let probe = null;

  function settle(reason) {
    if (!probe) {
      return;
    }

    const current = probe;
    probe = null;
    clearTimeout(current.timeout);
    writeLog({
      type: 'probe-finish',
      stage: current.stage,
      reason,
      elapsed: Date.now() - current.startedAt,
      kittyQueryResponse: current.kittyQueryResponse,
      deviceAttributes: current.deviceAttributes,
    });
    current.resolve({
      reason,
      kittyQueryResponse: current.kittyQueryResponse,
      deviceAttributes: current.deviceAttributes,
    });
  }

  stream.on('data', (data) => {
    writeLog({ type: 'read', bytes: toHex(data) });

    if (!probe) {
      return;
    }

    const da = parseDeviceAttributes(data);
    if (da) {
      probe.deviceAttributes = da;
      writeLog({ type: 'device-attributes', stage: probe.stage, deviceAttributes: da });
      if (probe.kittyQueryResponse !== null) {
        settle('complete');
      }
    }
  });

  async function runProbe(stage) {
    if (probe) {
      settle('preempt');
    }

    return new Promise((resolve) => {
      probe = {
        stage,
        startedAt: Date.now(),
        kittyQueryResponse: null,
        deviceAttributes: null,
        resolve,
        timeout: setTimeout(() => settle('timeout'), 200),
      };

      writeLog({ type: 'probe-start', stage });
      process.stdout.write(QUERY_KITTY);
      process.stdout.write(QUERY_DA1);
    });
  }

  try {
    await runProbe('before_enable');
    process.stdout.write(ENABLE_FOCUS);
    process.stdout.write(ENABLE_INBAND_RESIZE);
    process.stdout.write(ENABLE_BRACKETED_PASTE);
    process.stdout.write(ENABLE_KITTY_KEYBOARD);
    await runProbe('after_enable');
  } finally {
    cleanup(stream);
  }
}

main().catch((error) => {
  writeLog({ type: 'fatal', message: error && error.message ? error.message : String(error) });
  process.exitCode = 1;
});
