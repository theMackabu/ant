import { parentPort } from 'ant:sandbox';

parentPort.on('message', message => {
  for (let index = 0; index < message.count; index++) parentPort.send({ type: 'item', index });
  parentPort.send({ type: 'done', count: message.count });
  if (message.exit) process.exit(0);
});

setTimeout(() => parentPort.send({ type: 'ready' }), 100);
