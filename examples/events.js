console.log('Example 1: Global Events');
addEventListener('appReady', () => {
  console.log('  App is ready!');
});
dispatchEvent('appReady');

console.log('\nExample 2: Custom Event Data');
addEventListener('userLogin', event => {
  console.log('  User logged in:', event.detail.username);
  console.log('  Login time:', event.detail.timestamp);
});
dispatchEvent('userLogin', {
  username: 'alice',
  timestamp: Date.now()
});

console.log('\nExample 3: Object Events');
const button = createEventTarget();
button.name = 'MyButton';

button.addEventListener('click', event => {
  console.log('  Button clicked!');
  console.log('  Button name:', event.target.name);
});

button.dispatchEvent('click');
button.dispatchEvent('click');

console.log('\nExample 4: Multiple Event Targets');
const user = createEventTarget();
user.name = 'Alice';

const socket = createEventTarget();
socket.id = 'socket-123';

user.addEventListener('statusChange', event => {
  console.log('  User status changed:', event.detail.status);
});

socket.addEventListener('message', event => {
  console.log('  Socket message:', event.detail.data);
});

user.dispatchEvent('statusChange', { status: 'online' });
socket.dispatchEvent('message', { data: 'Hello!' });

console.log('\nExample 5: Once Listeners');
const startup = createEventTarget();
let initCount = 0;

startup.addEventListener(
  'init',
  () => {
    initCount++;
    console.log('  Initialization running... (count:', initCount + ')');
  },
  { once: true }
);

startup.dispatchEvent('init');
startup.dispatchEvent('init');
startup.dispatchEvent('init');
console.log('  Total init calls:', initCount);

console.log('\nExample 6: Event Emitter Pattern');
class MyEmitter {
  constructor() {
    const target = createEventTarget();
    this.on = target.addEventListener.bind(target);
    this.off = target.removeEventListener.bind(target);
    this.emit = target.dispatchEvent.bind(target);
  }
}

const emitter = new MyEmitter();

emitter.on('data', event => {
  console.log('  Data received:', event.detail);
});

emitter.emit('data', { value: 42, type: 'number' });
emitter.emit('data', { value: 'hello', type: 'string' });

console.log('\nExample 7: Removing Listeners');
const temp = createEventTarget();

function handler(event) {
  console.log('  Handler called with:', event.detail);
}

temp.addEventListener('test', handler);
temp.dispatchEvent('test', 'first call');

temp.removeEventListener('test', handler);
temp.dispatchEvent('test', 'second call (not shown)');

console.log('  Handler was removed successfully');
