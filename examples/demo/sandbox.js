import { Sandbox } from 'ant:sandbox';

const verbose = process.argv.includes('--verbose');
const base = { mount: '.:/workspace', verbose };

const sandbox = new Sandbox(base);
const writableSandbox = new Sandbox({ ...base, write: 'tmp:/tmp' });

async function showError(label, fn) {
  try {
    await fn();
  } catch (error) {
    console.log(`${label}: ${error.name}: ${error.message}`);
  }
}

try {
  const value = await sandbox.eval('export default 1 + 1');
  console.log(`sandbox eval: ${value}`);

  console.log('\nsandbox run:');
  const code = await sandbox.run('examples/demo/pi.js');
  console.log(`sandbox exited with code ${code}`);

  console.log('\nsandbox writable mount:');
  const writeCode = await writableSandbox.run('examples/demo/writable_mount.js');
  console.log(`writable sandbox exited with code ${writeCode}`);

  console.log('\nsandbox guest error:');
  await showError('eval threw', () => sandbox.eval('throw new TypeError("boom")'));

  console.log('\nsandbox script exit:');
  await showError('run failed', () => sandbox.run('does-not-exist.js'));
} finally {
  await sandbox.close();
  await writableSandbox.close();
}
