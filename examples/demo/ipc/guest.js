import { parentPort } from 'ant:sandbox';

let requests = 0;

parentPort.on('message', message => {
  requests++;

  switch (message.type) {
    case 'ping':
      parentPort.send({ type: 'pong', requests });
      break;
    case 'add':
      parentPort.send({
        type: 'result',
        value: message.left + message.right,
        requests
      });
      break;
    default:
      throw new Error(`unknown host message: ${message.type}`);
  }
});

setTimeout(() => {
  parentPort.send({ type: 'ready' });
}, 100);
