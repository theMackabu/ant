import * as fs from 'node:fs';
import * as fsp from 'node:fs/promises';
import path from 'node:path';

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

async function readFileCallback(url, encoding) {
  return await new Promise((resolve, reject) => {
    fs.readFile(url, encoding, (error, data) => {
      if (error) reject(error);
      else resolve(data);
    });
  });
}

async function main() {
  const dirPath = path.join(import.meta.dirname, '.fs url tmp');
  const filePath = path.join(dirPath, 'url file.txt');
  const content = 'hello from file URL reads';

  fs.rmSync(dirPath, { recursive: true, force: true });
  fs.mkdirSync(dirPath, { recursive: true });
  fs.writeFileSync(filePath, content);
  const fileUrl = new URL(`file://${encodeURI(filePath)}`);

  try {
    const syncContent = fs.readFileSync(fileUrl, 'utf8');
    assert(syncContent === content, `expected sync read to match, got ${JSON.stringify(syncContent)}`);

    const callbackContent = await readFileCallback(fileUrl, 'utf8');
    assert(callbackContent === content, `expected callback read to match, got ${JSON.stringify(callbackContent)}`);

    const promiseContent = await fsp.readFile(fileUrl, 'utf8');
    assert(promiseContent === content, `expected promise read to match, got ${JSON.stringify(promiseContent)}`);

    console.log('fs readFile accepts file URLs');
  } finally {
    fs.rmSync(dirPath, { recursive: true, force: true });
  }
}

await main();
