function assert(condition, message) {
  if (!condition) throw new Error(message || "assertion failed");
}

function churn(count) {
  const values = [];
  for (let i = 0; i < count; i++) values.push({ i });
  return values.length;
}

function setExisting(target, value) {
  target.value = value;
}

function setOverflow(target, value) {
  target.p12 = value;
}

function addValue(target, value) {
  target.added = value;
}

const existingTarget = { value: 0 };
const overflowTarget = {
  p0: 0,
  p1: 1,
  p2: 2,
  p3: 3,
  p4: 4,
  p5: 5,
  p6: 6,
  p7: 7,
  p8: 8,
  p9: 9,
  p10: 10,
  p11: 11,
  p12: 12,
};
const transitionTarget = {};

for (let i = 0; i < 500; i++) {
  setExisting(existingTarget, i);
  setOverflow(overflowTarget, i);
  addValue({}, i);
}

churn(400_000);
churn(400_000);

setExisting(existingTarget, { marker: "in-object" });
setOverflow(overflowTarget, { marker: "overflow" });
addValue(transitionTarget, { marker: "transition" });

churn(400_000);
churn(400_000);

assert(existingTarget.value.marker === "in-object", "in-object JIT store lost young value");
assert(overflowTarget.p12.marker === "overflow", "overflow JIT store lost young value");
assert(transitionTarget.added.marker === "transition", "transition JIT store lost young value");

console.log("JIT put-field GC barrier tests passed");
