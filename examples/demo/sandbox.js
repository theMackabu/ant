import { Sandbox } from 'ant:sandbox';

const sandbox = new Sandbox({ mount: '.:/workspace' });

try {
  console.log('sandbox eval:');
  await sandbox.eval('export default 1 + 1');

  console.log('\n\nsandbox run:');
  const code = await sandbox.run('examples/demo/pi.js');

  console.log(`\nsandbox exited with code ${code}`);
} finally {
  await sandbox.close();
}
