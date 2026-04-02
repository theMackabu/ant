const fs = require('node:fs');
const sourcePath = 'tests/.fs_stream_source.txt';
const copyPath = 'tests/.fs_stream_copy.txt';
const content = 'hello from fs streams\nline two';

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
  const written = fs.readFileSync(sourcePath, 'utf8');
  if (written !== content) fail(new Error(`unexpected write content: ${written}`));

  const reader = fs.createReadStream(sourcePath);
  if (!(reader instanceof fs.ReadStream)) fail(new Error('createReadStream() did not return fs.ReadStream'));

  let readBack = '';
  reader.on('error', fail);
  reader.on('data', chunk => {
    readBack += chunk.toString();
  });
  reader.on('end', () => {
    if (readBack !== content) fail(new Error(`unexpected read content: ${readBack}`));

    const pipedReader = fs.createReadStream(sourcePath);
    const pipedWriter = fs.createWriteStream(copyPath);
    pipedReader.on('error', fail);
    pipedWriter.on('error', fail);
    pipedWriter.on('finish', () => {
      const copied = fs.readFileSync(copyPath, 'utf8');
      if (copied !== content) fail(new Error(`unexpected piped content: ${copied}`));

      fs.unlinkSync(sourcePath);
      fs.unlinkSync(copyPath);
      console.log('fs stream test passed');
    });
    pipedReader.pipe(pipedWriter);
  });
});

writer.write('hello ');
writer.end('from fs streams\nline two');
