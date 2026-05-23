import { Sandbox } from 'ant:sandbox';

const sandbox = new Sandbox({ mount: '.:/workspace' });

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

  console.log('\nsandbox guest error:');
  await showError('eval threw', () => sandbox.eval('throw new TypeError("boom")'));

  console.log('\nsandbox script exit:');
  await showError('run failed', () => sandbox.run('does-not-exist.js'));
} finally {
  await sandbox.close();
}
