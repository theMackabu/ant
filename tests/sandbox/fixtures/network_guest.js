import { parentPort } from 'ant:sandbox';

parentPort.on('message', async message => {
  const sizes = [];
  for (const size of message.sizes) {
    const response = await fetch(`http://10.0.2.2:${message.port}/body?${size}`);
    sizes.push((await response.arrayBuffer()).byteLength);
  }

  const echoed = await fetch(`http://10.0.2.2:${message.port}/echo`, { method: 'POST', body: 'x'.repeat(message.upload) });
  const upload = Number(await echoed.text());

  let blocked = 'reachable';
  try {
    await fetch(`http://10.0.2.2:${message.blocked}/body?1`);
  } catch {
    blocked = 'refused';
  }
  parentPort.send({ type: 'result', sizes, upload, blocked });
});

setTimeout(() => parentPort.send({ type: 'ready' }), 100);
