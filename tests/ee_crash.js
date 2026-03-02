import { EventEmitter } from 'ant:events';
const ee = new EventEmitter();
console.log('on type:', typeof ee.on);
ee.on('x', () => {});
console.log('done');
