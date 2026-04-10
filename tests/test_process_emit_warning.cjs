const { spawnSync } = require('child_process');

function assertIncludes(haystack, needle, label) {
  if (!haystack.includes(needle)) {
    throw new Error(`${label}: expected ${JSON.stringify(haystack)} to include ${JSON.stringify(needle)}`);
  }
}

function assertNotIncludes(haystack, needle, label) {
  if (haystack.includes(needle)) {
    throw new Error(`${label}: expected ${JSON.stringify(haystack)} not to include ${JSON.stringify(needle)}`);
  }
}

function assertMatch(haystack, pattern, label) {
  if (!pattern.test(haystack)) {
    throw new Error(`${label}: expected ${JSON.stringify(haystack)} to match ${pattern}`);
  }
}

function run(source) {
  const result = spawnSync(process.execPath, ['-e', source]);
  if (result.error) throw result.error;
  if (result.status !== 0) {
    throw new Error(`child exited ${result.status}\nstdout:\n${result.stdout}\nstderr:\n${result.stderr}`);
  }
  return {
    stdout: String(result.stdout),
    stderr: String(result.stderr),
  };
}

const withDetail = run(`
  process.emitWarning('Something happened!', {
    code: 'Custom_Warning',
    detail: 'Additional information about warning'
  });
`);

assertMatch(
  withDetail.stderr,
  /\\(ant:\\d+\\) \\[Custom_Warning\\] Warning: Something happened!\\n/,
  'emitWarning formats code, type, and message'
);
assertIncludes(
  withDetail.stderr,
  'Additional information about warning\n',
  'emitWarning prints detail'
);
assertNotIncludes(
  withDetail.stderr,
  '[Custom_Warning] : Something happened!',
  'emitWarning avoids the old empty type formatting'
);

const listenerPayload = run(`
  process.on('warning', warning => {
    console.log([warning.name, warning.message, warning.code, warning.detail].join('|'));
  });
  process.emitWarning('msg', { code: 'C', detail: 'd' });
`);

assertIncludes(listenerPayload.stderr, '[C] Warning: msg\n', 'emitWarning still writes default warning output');
assertIncludes(listenerPayload.stdout, 'Warning|msg|C|d\n', 'emitWarning emits a warning object');

console.log('process.emitWarning formatting ok');
