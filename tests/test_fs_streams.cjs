const fs = require('node:fs');
const { Buffer } = require('node:buffer');
const sourcePath = '/tmp/ant_fs_stream_seq_source.bin';
const copyPath = '/tmp/ant_fs_stream_seq_copy.bin';
const content = Buffer.alloc(70000);

for (let i = 0; i < content.length; i++) {
  content[i] = i & 255;
}

function sameBuffer(left, right) {
  if (!left || !right || left.length !== right.length) return false;
  for (let i = 0; i < left.length; i++) {
    if (left[i] !== right[i]) return false;
  }
  return true;
}

try {
  fs.unlinkSync(sourcePath);
} catch {}
try {
  fs.unlinkSync(copyPath);
} catch {}

function fail(error) {
  console.error(error);
  process.exit(1);
}

const writer = fs.createWriteStream(sourcePath);
if (!(writer instanceof fs.WriteStream)) throw new Error('createWriteStream() did not return fs.WriteStream');
writer.on('error', fail);
writer.on('finish', () => {
  const written = fs.readFileSync(sourcePath);
  if (!sameBuffer(written, content)) fail(new Error(`unexpected write content length: ${written.length}`));

  const reader = fs.createReadStream(sourcePath);
  if (!(reader instanceof fs.ReadStream)) fail(new Error('createReadStream() did not return fs.ReadStream'));

  const readBack = [];
  reader.on('error', fail);
  reader.on('data', chunk => {
    readBack.push(chunk);
  });
  reader.on('end', () => {
    if (!sameBuffer(Buffer.concat(readBack), content)) fail(new Error('unexpected read content'));

    const pipedReader = fs.createReadStream(sourcePath);
    const pipedWriter = fs.createWriteStream(copyPath);
    pipedReader.on('error', fail);
    pipedWriter.on('error', fail);
    pipedWriter.on('finish', () => {
      const copied = fs.readFileSync(copyPath);
      if (!sameBuffer(copied, content)) fail(new Error(`unexpected piped content length: ${copied.length}`));

      fs.unlinkSync(sourcePath);
      fs.unlinkSync(copyPath);
      console.log('fs stream test passed');
    });
    pipedReader.pipe(pipedWriter);
  });
});

writer.write(content.subarray(0, 32768));
writer.end(content.subarray(32768));
