import {
  mkdirSync,
  readFileSync,
  renameSync,
  rmdirSync,
  statSync,
  unlinkSync,
  writeFileSync,
} from 'ant:fs';

const dir = '/tmp/ant-writable-demo';
const path = `${dir}/hello.txt`;
const renamed = `${dir}/renamed.txt`;

try {
  mkdirSync(dir);
} catch {}

writeFileSync(path, 'hello from a writable sandbox mount');
renameSync(path, renamed);

const text = readFileSync(renamed, 'utf8');
const stat = statSync(renamed);

console.log(text);
console.log(`bytes=${stat.size}`);

unlinkSync(renamed);
rmdirSync(dir);
