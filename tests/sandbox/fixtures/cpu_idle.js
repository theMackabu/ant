import { parentPort } from 'ant:sandbox';

setTimeout(() => parentPort.send({ type: 'ready' }), 100);
setInterval(() => {}, 1_000);
