import { Sandbox } from 'ant:sandbox';

const sandbox = new Sandbox({ mount: '.:/workspace' });

try {
  const value = await sandbox.eval('export default 1 + 1');
  console.log(`sandbox eval: ${value}`);

  console.log('\nsandbox run:');
  const code = await sandbox.run('examples/demo/pi.js');

  console.log(`\nsandbox exited with code ${code}`);
} finally {
  await sandbox.close();
}
