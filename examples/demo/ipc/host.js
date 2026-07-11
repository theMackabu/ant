import { Sandbox } from 'ant:sandbox';

const sandbox = new Sandbox({ memory: '128mb' });
const running = sandbox.run('examples/demo/ipc/guest.js');

console.log('host received:', await sandbox.once('ready'));

sandbox.send({ type: 'ping' });
console.log('host received:', await sandbox.receive());

sandbox.send({ type: 'add', left: 20, right: 22 });
for await (const message of sandbox.messages) {
  console.log('host received:', message);
  if (message.type === 'result') break;
}

await sandbox.close();
await running;
