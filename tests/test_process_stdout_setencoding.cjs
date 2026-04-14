function assert(condition, message) {
  if (!condition) throw new Error(message);
}

assert(typeof process.stdout.setEncoding === 'function', 'process.stdout.setEncoding should exist');
assert(typeof process.stderr.setEncoding === 'function', 'process.stderr.setEncoding should exist');

const stdoutReturn = process.stdout.setEncoding('utf8');
const stderrReturn = process.stderr.setEncoding('utf8');

assert(stdoutReturn === process.stdout, 'stdout.setEncoding should return process.stdout');
assert(stderrReturn === process.stderr, 'stderr.setEncoding should return process.stderr');
assert(process.stdout.readableEncoding === 'utf8', 'stdout readableEncoding should be utf8');
assert(process.stderr.readableEncoding === 'utf8', 'stderr readableEncoding should be utf8');

console.log('process stdout/stderr setEncoding ok');
