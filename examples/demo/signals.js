import { constants } from 'ant:os';

let counter = 0;

Ant.signal(constants.signals.SIGINT, function (signum) {
  console.log('\nreceived SIGINT (signal', signum, ')');
  console.log('counter reached:', counter);
  console.log('shutting down gracefully...');
});

Ant.signal(constants.signals.SIGTERM, function (signum) {
  console.log('\nreceived SIGTERM (signal', signum, ')');
  console.log('counter reached:', counter);
  console.log('terminating...');
});

console.log('starting counter...');
console.log('press Ctrl+C to trigger graceful shutdown');

for (;;) {
  counter++; Ant.usleep(5000);
  if (counter % 1000000 === 0) console.log('counter:', counter);
}
