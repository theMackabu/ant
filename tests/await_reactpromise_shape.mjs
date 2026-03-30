function ReactPromiseShape(value) {
  this.value = value;
}

ReactPromiseShape.prototype = Object.create(Promise.prototype);
ReactPromiseShape.prototype.constructor = ReactPromiseShape;
let lastThis = null;
ReactPromiseShape.prototype.then = function(resolve, _reject) {
  lastThis = this;
  resolve(this.value);
};

async function main() {
  const first = new ReactPromiseShape([1]);
  console.log(`first.then.type:${typeof first.then}`);
  console.log(`first.hasOwn.then:${Object.prototype.hasOwnProperty.call(first, 'then')}`);
  console.log(`proto.hasOwn.then:${Object.prototype.hasOwnProperty.call(ReactPromiseShape.prototype, 'then')}`);
  console.log(`first.then.eq.proto:${first.then === ReactPromiseShape.prototype.then}`);

  const awaited = await first;
  console.log(`await.protoThen.isUndefined:${awaited === undefined}`);
  console.log(`await.protoThen.isArray:${Array.isArray(awaited)}`);
  console.log(`await.protoThen.0:${String(awaited?.[0])}`);
  console.log(`await.protoThen.thisEqInstance:${lastThis === first}`);

  const second = new ReactPromiseShape([2]);
  lastThis = null;
  const resolved = await Promise.resolve(second);
  console.log(`promiseResolve.protoThen.isUndefined:${resolved === undefined}`);
  console.log(`promiseResolve.protoThen.isArray:${Array.isArray(resolved)}`);
  console.log(`promiseResolve.protoThen.0:${String(resolved?.[0])}`);
  console.log(`promiseResolve.protoThen.thisEqInstance:${lastThis === second}`);
}

await main();
