var SequenceProto = function (start) {
  this.item = start;
  return this;
};

SequenceProto.prototype.next = function () {
  var temp = this.item;
  this.item = temp + 2;
  return temp;
};

class SequenceClass {
  constructor(start) {
    this.item = start;
  }
  next() {
    const temp = this.item;
    this.item = temp + 2;
    return temp;
  }
}

const ITERATIONS = 1_000_000;
const WARMUP = 100_000;

function runProto() {
  const seq = new SequenceProto(1);
  let num = 0;
  for (let i = 0; i < ITERATIONS; i++) {
    num = seq.next();
  }
  return num;
}

function runClass() {
  const seq = new SequenceClass(1);
  let num = 0;
  for (let i = 0; i < ITERATIONS; i++) {
    num = seq.next();
  }
  return num;
}

for (let i = 0; i < WARMUP; i++) {
  new SequenceProto(1).next();
  new SequenceClass(1).next();
}

const runs = 10;
const protoTimes = [];
const classTimes = [];

for (let i = 0; i < runs; i++) {
  const t1 = performance.now();
  runProto();
  protoTimes.push(performance.now() - t1);

  const t2 = performance.now();
  runClass();
  classTimes.push(performance.now() - t2);
}

const avg = arr => arr.reduce((a, b) => a + b, 0) / arr.length;

console.log(`Prototype avg: ${avg(protoTimes).toFixed(3)}ms`);
console.log(`Class avg:     ${avg(classTimes).toFixed(3)}ms`);
console.log(`Difference:    ${Math.abs(avg(protoTimes) - avg(classTimes)).toFixed(3)}ms`);
console.log(`Winner:        ${avg(protoTimes) < avg(classTimes) ? 'Prototype' : 'Class'}`);
