let failed = 0;

function writeTarget(obj, value) {
  obj.target = value;
  return obj.target;
}

function readTarget(obj) {
  return obj.target;
}

function readInheritedTarget(obj) {
  return obj.inheritedTarget;
}

function closureChurn() {
  for (let i = 0; i < 150000; i++) {
    const dead = () => i;
    if (i === -1) console.log(dead());
  }
}

for (let round = 0; round < 24; round++) {
  let old = { pad: 1, target: 2, tail: 3 };
  delete old.pad;

  for (let i = 0; i < 300; i++) {
    if (writeTarget(old, i) !== i || readTarget(old) !== i) failed++;
  }

  let oldProto = { pad: 1, inheritedTarget: 2, tail: 3 };
  delete oldProto.pad;
  let oldChild = Object.create(oldProto);
  for (let i = 0; i < 300; i++) {
    if (readInheritedTarget(oldChild) !== 2) failed++;
  }

  old = null;
  oldChild = null;
  oldProto = null;
  closureChurn();

  const fresh = { target: 10, decoy: 20, tail: 30 };
  delete fresh.tail;
  const result = writeTarget(fresh, 99);
  if (result !== 99 || fresh.target !== 99 || fresh.decoy !== 20) {
    console.log(
      `FAIL round ${round}: result=${result} target=${fresh.target} decoy=${fresh.decoy}`
    );
    failed++;
    break;
  }

  const freshProto = { inheritedTarget: 40, decoy: 50, tail: 60 };
  delete freshProto.tail;
  const freshChild = Object.create(freshProto);
  const inherited = readInheritedTarget(freshChild);
  if (inherited !== 40 || freshProto.decoy !== 50) {
    console.log(
      `FAIL inherited round ${round}: value=${inherited} decoy=${freshProto.decoy}`
    );
    failed++;
    break;
  }
}

if (failed) process.exit(1);
console.log('OK');
