function assert(condition, message) {
  if (!condition) throw new Error(message);
}

for (const [name, stream] of [
  ['stdout', process.stdout],
  ['stderr', process.stderr],
]) {
  assert(typeof stream.setEncoding === 'function', `${name}.setEncoding should exist`);

  const returned = stream.setEncoding('utf8');
  assert(returned === stream, `${name}.setEncoding should return the stream`);
  assert(stream.encoding === 'utf8', `${name}.encoding should be utf8`);
}

console.log('process stdout/stderr setEncoding ok');
