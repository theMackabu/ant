const { spawnSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

const keywordKeys = {
  default: 'default value',
  true: 'true value',
  null: 'null value',
  class: 'class value',
};

assert(keywordKeys.default === 'default value', 'dot access should allow default as property name');
assert(keywordKeys.true === 'true value', 'dot access should allow true as property name');
assert(keywordKeys.null === 'null value', 'dot access should allow null as property name');
assert(keywordKeys.class === 'class value', 'dot access should allow class as property name');

const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), 'ant-dot-property-syntax-'));

function runInvalid(file, source) {
  const scriptPath = path.join(tmpDir, file);
  fs.writeFileSync(scriptPath, source);
  const result = spawnSync(process.execPath, [scriptPath], { encoding: 'utf8' });
  if (result.error) throw result.error;
  assert(result.status !== 0, `${file} should fail to parse`);
  assert(result.stderr.includes('SyntaxError'), `${file} should report SyntaxError\n${result.stderr}`);
  assert(result.stderr.includes("Unexpected token '&'"), `${file} should reject the punctuator after dot\n${result.stderr}`);
  assert(!result.stderr.includes('ReferenceError'), `${file} should not execute as a bitwise expression\n${result.stderr}`);
}

runInvalid(
  'invalid-dot-property.cjs',
  "const person = { '&weird property': 'YYCJS' };\nperson.&weird property;\n"
);

runInvalid(
  'invalid-optional-dot-property.cjs',
  "const person = { '&weird property': 'YYCJS' };\nperson?.&weird property;\n"
);

console.log('dot property syntax test passed');
