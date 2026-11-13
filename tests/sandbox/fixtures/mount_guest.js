import fs from 'node:fs';
import { parentPort } from 'ant:sandbox';

function attempt(action) {
  try {
    action();
    return 'ok';
  } catch (err) {
    return err.code || String(err);
  }
}

parentPort.on('message', () => {
  const result = { type: 'result' };
  result.seed = fs.readFileSync('/data/seed.txt', 'utf8');
  fs.mkdirSync('/data/dir');
  fs.writeFileSync('/data/dir/file.txt', 'x'.repeat(300000));
  result.size = fs.statSync('/data/dir/file.txt').size;
  fs.appendFileSync('/data/dir/file.txt', 'tail');
  fs.renameSync('/data/dir/file.txt', '/data/dir/renamed.txt');
  fs.symlinkSync('dir/renamed.txt', '/data/link');
  result.link = fs.readlinkSync('/data/link');
  result.viaLink = fs.readFileSync('/data/link', 'utf8').length;
  result.listing = fs.readdirSync('/data').sort();
  result.notEmpty = attempt(() => fs.rmdirSync('/data/dir'));
  result.missing = attempt(() => fs.readFileSync('/data/nope'));
  result.exists = attempt(() => fs.mkdirSync('/data/dir'));
  result.escape = attempt(() => fs.readFileSync('/data/escape/secret'));
  result.readonlyRead = fs.readFileSync('/ro/seed.txt', 'utf8');
  result.readonlyWrite = attempt(() => fs.writeFileSync('/ro/new.txt', 'x'));
  fs.unlinkSync('/data/seed.txt');
  parentPort.send(result);
});

setTimeout(() => parentPort.send({ type: 'ready' }), 100);
