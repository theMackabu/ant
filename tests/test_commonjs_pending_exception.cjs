const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createRequire } = require('node:module');
const { pathToFileURL } = require('node:url');

async function main() {
  const dir = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cjs-pending-')));
  const localRequire = createRequire(path.join(dir, 'entry.cjs'));
  let index = 0;
  try {
    for (const mode of ['require', 'import']) {
      const file = path.join(dir, `case-${index++}.cjs`);
      fs.writeFileSync(file, `
        globalThis.__cjsPendingModule = module;
        module.exports = { phase: 'failed' };
        Object.defineProperty(module, 'loaded', {
          value: false, writable: false
        });
      `);
      let caught = false;
      try {
        if (mode === 'require') localRequire(file);
        else await import(pathToFileURL(file).href);
      } catch (error) {
        caught = true;
        assert(error instanceof TypeError);
      }
      assert.strictEqual(caught, true, `${mode} must report the failed loaded write`);
      assert.strictEqual(Object.hasOwn(localRequire.cache, file), false);
      const failedModule = globalThis.__cjsPendingModule;
      if (failedModule.parent) assert.strictEqual(failedModule.parent.children.includes(failedModule), false);

      fs.writeFileSync(file, 'module.exports = { phase: "recovered" };');
      if (mode === 'require') {
        // A failed require must not publish an ESM namespace as loaded.
        const imported = await import(pathToFileURL(file).href);
        assert.strictEqual(imported.default.phase, 'recovered');
      }
      assert.strictEqual(localRequire(file).phase, 'recovered');
      assert.strictEqual(localRequire.cache[file].loaded, true);
    }

    const first = new Error('module body failure');
    globalThis.__cjsPendingReason = first;
    const file = path.join(dir, 'existing-error.cjs');
    fs.writeFileSync(file, `
      Object.defineProperty(module, 'exports', {
        get() { throw new Error('secondary exports failure'); }
      });
      throw globalThis.__cjsPendingReason;
    `);
    assert.throws(() => localRequire(file), error => error === first);
    assert.strictEqual(Object.hasOwn(localRequire.cache, file), false);
    console.log('CommonJS pending exceptions propagate and clean up caches');
  } finally {
    delete globalThis.__cjsPendingReason;
    delete globalThis.__cjsPendingModule;
    for (const key of Object.keys(localRequire.cache)) {
      if (key.startsWith(dir + path.sep)) delete localRequire.cache[key];
    }
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
