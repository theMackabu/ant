const path = require('node:path');
const { spawnSync } = require('node:child_process');

function loadNative(name) {
  const dirs = [`./prebuilds/${process.platform}-${process.arch}`];
  let lastError;

  for (const dir of dirs) {
    try {
      return { dir, module: require(`${dir}/${name}.node`) };
    } catch (error) {
      lastError = error;
    }
  }
  throw lastError;
}

const native = loadNative('binding');
const nativePath = require.resolve(`${native.dir}/binding.node`);
const helperPath = path.resolve(__dirname, native.dir, 'helper');
const helper = spawnSync(helperPath, [], { encoding: 'utf8' });
if (helper.status !== 0) throw new Error(`helper failed: ${helper.stderr}`);

module.exports = {
  dirname: __dirname,
  helper: helper.stdout.trim(),
  helperPath,
  native: native.module.value,
  nativePath,
};
