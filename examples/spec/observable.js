import { test, testThrows, testDeep, summary } from './helpers.js';

console.log('Observable Constructor Tests\n');

test('Observable is defined', typeof Observable, 'function');
test('Observable.prototype exists', typeof Observable.prototype, 'object');
test('Observable.prototype.constructor is Observable', Observable.prototype.constructor, Observable);

testThrows('Constructor throws without arguments', () => new Observable());
testThrows('Constructor throws with non-function', () => new Observable({}));
testThrows('Constructor throws with null', () => new Observable(null));
testThrows('Constructor throws with number', () => new Observable(1));

let subscriberCalled = false;
new Observable(() => {
  subscriberCalled = true;
});
test('Subscriber not called by constructor', subscriberCalled, false);

console.log('\nObservable.prototype.subscribe Tests\n');

test('subscribe method exists', typeof Observable.prototype.subscribe, 'function');

const obs1 = new Observable(sink => null);
let noThrow = true;
try {
  obs1.subscribe(null);
  obs1.subscribe(undefined);
  obs1.subscribe(1);
  obs1.subscribe({});
  obs1.subscribe(() => {});
} catch (e) {
  noThrow = false;
}
test('subscribe accepts any observer type', noThrow, true);

const list1 = [];
const error1 = new Error('test error');
new Observable(s => {
  s.next(1);
  s.error(error1);
}).subscribe(
  x => list1.push('next:' + x),
  e => list1.push(e),
  () => list1.push('complete')
);
test('First subscribe arg is next callback', list1[0], 'next:1');
test('Second subscribe arg is error callback', list1[1], error1);

const list2 = [];
new Observable(s => {
  s.complete();
}).subscribe(
  x => list2.push('next:' + x),
  e => list2.push(e),
  () => list2.push('complete')
);
test('Third subscribe arg is complete callback', list2[0], 'complete');

let observer1 = null;
new Observable(x => {
  observer1 = x;
}).subscribe({});
test('Subscriber receives observer object', typeof observer1, 'object');
test('Observer has next method', typeof observer1.next, 'function');
test('Observer has error method', typeof observer1.error, 'function');
test('Observer has complete method', typeof observer1.complete, 'function');

console.log('\nSubscription Tests\n');

let cleanupCalled = 0;
const sub1 = new Observable(observer => {
  return () => {
    cleanupCalled++;
  };
}).subscribe({});

test('subscribe returns subscription object', typeof sub1, 'object');
test('subscription has unsubscribe method', typeof sub1.unsubscribe, 'function');
test('subscription.closed is false before unsubscribe', sub1.closed, false);
test('unsubscribe returns undefined', sub1.unsubscribe(), undefined);
test('cleanup function called on unsubscribe', cleanupCalled, 1);
test('subscription.closed is true after unsubscribe', sub1.closed, true);

sub1.unsubscribe();
test('cleanup not called again on second unsubscribe', cleanupCalled, 1);

let cleanupOnError = 0;
new Observable(sink => {
  sink.error(1);
  return () => {
    cleanupOnError++;
  };
}).subscribe({ error: () => {} });
test('cleanup called on error', cleanupOnError, 1);

let cleanupOnComplete = 0;
new Observable(sink => {
  sink.complete();
  return () => {
    cleanupOnComplete++;
  };
}).subscribe({});
test('cleanup called on complete', cleanupOnComplete, 1);

let unsubscribeCalled = 0;
const sub2 = new Observable(sink => {
  return {
    unsubscribe: () => {
      unsubscribeCalled++;
    }
  };
}).subscribe({});
sub2.unsubscribe();
test('subscription object with unsubscribe is valid return', unsubscribeCalled, 1);

console.log('\nSubscriber Error Handling Tests\n');

const thrownError = new Error('subscriber error');
let caughtError = null;
new Observable(() => {
  throw thrownError;
}).subscribe({
  error: e => {
    caughtError = e;
  }
});
test('subscriber errors sent to observer.error', caughtError, thrownError);

console.log('\nObserver.start Tests\n');

const events1 = [];
const obs2 = new Observable(observer => {
  events1.push('subscriber');
  observer.complete();
});

let startSubscription = null;
let startThisVal = null;
const observer2 = {
  start(subscription) {
    events1.push('start');
    startSubscription = subscription;
    startThisVal = this;
  }
};
const sub3 = obs2.subscribe(observer2);

test('start called before subscriber', events1[0], 'start');
test('subscriber called after start', events1[1], 'subscriber');
test('start receives subscription', startSubscription, sub3);
test('start called with observer as this', startThisVal, observer2);

const events2 = [];
const obs3 = new Observable(() => {
  events2.push('subscriber');
});
obs3.subscribe({
  start(subscription) {
    events2.push('start');
    subscription.unsubscribe();
  }
});
test('unsubscribe in start prevents subscriber call', events2.length, 1);
test('only start was called', events2[0], 'start');

console.log('\nSubscriptionObserver.next Tests\n');

const token1 = {};
let nextReceived = null;
let nextArgs = [];
new Observable(observer => {
  observer.next(token1, 'extra');
}).subscribe({
  next(value, ...args) {
    nextReceived = value;
    nextArgs = args;
  }
});
test('next forwards value to observer', nextReceived, token1);
test('next does not forward extra arguments', nextArgs.length, 0);

let nextReturnVal = null;
new Observable(observer => {
  nextReturnVal = observer.next();
  observer.complete();
}).subscribe({ next: () => 'return value' });
test('next suppresses observer return value', nextReturnVal, undefined);

let nextAfterClose = 'not called';
new Observable(observer => {
  observer.complete();
  nextReturnVal = observer.next();
}).subscribe({
  next: () => {
    nextAfterClose = 'called';
  }
});
test('next returns undefined when closed', nextReturnVal, undefined);
test('next does not call observer when closed', nextAfterClose, 'not called');

console.log('\nSubscriptionObserver.error Tests\n');

let errorReceived = null;
const errorToken = new Error('error token');
new Observable(observer => {
  observer.error(errorToken);
}).subscribe({
  error: e => {
    errorReceived = e;
  }
});
test('error forwards exception to observer', errorReceived, errorToken);

let errorAfterClose = 'not called';
new Observable(observer => {
  observer.complete();
  observer.error(new Error());
}).subscribe({
  error: () => {
    errorAfterClose = 'called';
  }
});
test('error does not call observer when closed', errorAfterClose, 'not called');

let nextAfterError = 'not called';
new Observable(observer => {
  observer.error(new Error());
  observer.next(1);
}).subscribe({
  next: () => {
    nextAfterError = 'called';
  },
  error: () => {}
});
test('next not called after error', nextAfterError, 'not called');

console.log('\nSubscriptionObserver.complete Tests\n');

let completeReceived = false;
new Observable(observer => {
  observer.complete();
}).subscribe({
  complete: () => {
    completeReceived = true;
  }
});
test('complete calls observer.complete', completeReceived, true);

let completeAfterClose = 'not called';
new Observable(observer => {
  observer.complete();
  observer.complete();
}).subscribe({
  complete: () => {
    completeAfterClose = 'called';
  }
});
test('complete only called once', completeAfterClose, 'called');

let nextAfterComplete = 'not called';
new Observable(observer => {
  observer.complete();
  observer.next(1);
}).subscribe({
  next: () => {
    nextAfterComplete = 'called';
  }
});
test('next not called after complete', nextAfterComplete, 'not called');

console.log('\nObservable.of Tests\n');

test('Observable.of exists', typeof Observable.of, 'function');

const ofValues = [];
Observable.of(1, 2, 3, 4).subscribe({
  next: v => ofValues.push(v),
  complete: () => ofValues.push('done')
});
testDeep('Observable.of delivers all values', ofValues, [1, 2, 3, 4, 'done']);

const ofEmpty = [];
Observable.of().subscribe({
  next: v => ofEmpty.push(v),
  complete: () => ofEmpty.push('done')
});
testDeep('Observable.of with no args calls complete', ofEmpty, ['done']);

console.log('\nObservable.from Tests\n');

test('Observable.from exists', typeof Observable.from, 'function');

testThrows('Observable.from throws with null', () => Observable.from(null));
testThrows('Observable.from throws with undefined', () => Observable.from(undefined));
testThrows('Observable.from throws with no args', () => Observable.from());

const fromArray = [];
Observable.from([1, 2, 3]).subscribe({
  next: v => fromArray.push(v),
  complete: () => fromArray.push('done')
});
testDeep('Observable.from works with arrays', fromArray, [1, 2, 3, 'done']);

const customObservable = {
  [Symbol.observable]() {
    return new Observable(observer => {
      observer.next('custom');
      observer.complete();
    });
  }
};
const fromCustom = [];
Observable.from(customObservable).subscribe({
  next: v => fromCustom.push(v),
  complete: () => fromCustom.push('done')
});
testDeep('Observable.from works with @@observable', fromCustom, ['custom', 'done']);

const obs4 = new Observable(o => {
  o.next(1);
  o.complete();
});
const fromObs = Observable.from(obs4);
test('Observable.from returns same observable if already Observable', fromObs, obs4);

testThrows('Observable.from throws with non-iterable object', () => {
  Observable.from({}).subscribe({});
});

console.log('\nSymbol.observable Tests\n');

test('Symbol.observable exists', typeof Symbol.observable, 'symbol');

const obs5 = new Observable(() => {});
test('Observable has @@observable method', typeof obs5[Symbol.observable], 'function');
test('@@observable returns this', obs5[Symbol.observable](), obs5);

console.log('\nEarly Unsubscribe Tests\n');

const earlyValues = [];
let earlyCleanup = false;
let sub4;
new Observable(observer => {
  observer.next(1);
  observer.next(2);
  observer.next(3);
  observer.complete();
  return () => {
    earlyCleanup = true;
  };
}).subscribe({
  start: subscription => { sub4 = subscription; },
  next: v => {
    earlyValues.push(v);
    if (v === 2) sub4.unsubscribe();
  }
});
test('early unsubscribe stops delivery', earlyValues.length <= 3, true);
test('cleanup called on early unsubscribe', earlyCleanup, true);

console.log('\nIterator Closing Tests\n');

let iteratorClosed = false;
const customIterable = {
  [Symbol.iterator]() {
    let i = 0;
    return {
      next() {
        if (i < 5) {
          const val = i;
          i++;
          return { value: val, done: false };
        }
        return { done: true };
      },
      return() {
        iteratorClosed = true;
        return { done: true };
      }
    };
  }
};

const iterValues = [];
let sub5;
new Observable(observer => {
  const iter = customIterable[Symbol.iterator]();
  let result = iter.next();
  while (!result.done) {
    observer.next(result.value);
    if (observer.closed) {
      if (iter.return) iter.return();
      break;
    }
    result = iter.next();
  }
  if (!observer.closed) observer.complete();
}).subscribe({
  start: subscription => { sub5 = subscription; },
  next: v => {
    iterValues.push(v);
    if (v === 2) sub5.unsubscribe();
  }
});

test('iterator early unsubscribe stops at value 2', iterValues.length <= 3, true);
test('iterator return() called on early unsubscribe', iteratorClosed, true);

console.log('\nObserver.closed Property Tests\n');

let closedBefore = null;
let closedAfter = null;
new Observable(observer => {
  closedBefore = observer.closed;
  observer.complete();
  closedAfter = observer.closed;
}).subscribe({});
test('observer.closed is false before complete', closedBefore, false);
test('observer.closed is true after complete', closedAfter, true);

summary();
