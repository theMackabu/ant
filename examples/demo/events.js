console.log('Example 1: Global Events');
addEventListener('appReady', () => {
  console.log('  App is ready!');
});
dispatchEvent(new Event('appReady'));

console.log('\nExample 2: Custom Event Data');
addEventListener('userLogin', event => {
  console.log('  User logged in:', event.detail.username);
  console.log('  Login time:', event.detail.timestamp);
});
dispatchEvent(new CustomEvent('userLogin', {
  detail: {
    username: 'alice',
    timestamp: Date.now()
  }
}));

console.log('\nExample 3: Object Events');
const button = new EventTarget();
button.name = 'MyButton';

button.addEventListener('click', event => {
  console.log('  Button clicked!');
  console.log('  Button name:', event.target.name);
});

button.dispatchEvent(new Event('click'));
button.dispatchEvent(new Event('click'));

console.log('\nExample 4: Multiple Event Targets');
const user = new EventTarget();
user.name = 'Alice';

const socket = new EventTarget();
socket.id = 'socket-123';

user.addEventListener('statusChange', event => {
  console.log('  User status changed:', event.detail.status);
});

socket.addEventListener('message', event => {
  console.log('  Socket message:', event.detail.data);
});

user.dispatchEvent(new CustomEvent('statusChange', { detail: { status: 'online' } }));
socket.dispatchEvent(new CustomEvent('message', { detail: { data: 'Hello!' } }));

console.log('\nExample 5: Once Listeners');
const startup = new EventTarget();
let initCount = 0;

startup.addEventListener(
  'init',
  () => {
    initCount++;
    console.log('  Initialization running... (count:', initCount + ')');
  },
  { once: true }
);

startup.dispatchEvent(new Event('init'));
startup.dispatchEvent(new Event('init'));
startup.dispatchEvent(new Event('init'));
console.log('  Total init calls:', initCount);

console.log('\nExample 6: Event Emitter Pattern');
class MyEmitter {
  constructor() {
    const target = new EventTarget();
    this.on = target.addEventListener.bind(target);
    this.off = target.removeEventListener.bind(target);
    this.emit = target.dispatchEvent.bind(target);
  }
}

const emitter = new MyEmitter();

emitter.on('data', event => {
  console.log('  Data received:', event.detail);
});

emitter.emit(new CustomEvent('data', { detail: { value: 42, type: 'number' } }));
emitter.emit(new CustomEvent('data', { detail: { value: 'hello', type: 'string' } }));

console.log('\nExample 7: Removing Listeners');
const temp = new EventTarget();

function handler(event) {
  console.log('  Handler called with:', event.detail);
}

temp.addEventListener('test', handler);
temp.dispatchEvent(new CustomEvent('test', { detail: 'first call' }));

temp.removeEventListener('test', handler);
temp.dispatchEvent(new CustomEvent('test', { detail: 'second call (not shown)' }));

console.log('  Handler was removed successfully');

console.log('\nExample 8: CustomEvent');
const catFound = new CustomEvent('animalfound', {
  detail: {
    name: 'cat',
  },
});
const dogFound = new CustomEvent('animalfound', {
  detail: {
    name: 'dog',
  },
});

const element = new EventTarget();

element.addEventListener('animalfound', e => console.log(' ', e.detail.name, 'found'));

element.dispatchEvent(catFound);
element.dispatchEvent(dogFound);

console.log('\nExample 9: CustomEvent with global dispatch');
addEventListener('build:complete', e => {
  console.log('  Build finished:', e.detail.duration + 'ms,', e.detail.files, 'files');
});

dispatchEvent(new CustomEvent('build:complete', {
  detail: { duration: 142, files: 37 },
}));
