function makeChildHeavyFunction(childCount) {
  let src = 'return function run(seed) {\n';
  for (let i = 0; i < childCount; i++) src += `function child_${i}(x) { return x + ${i}; }\n`;
  src += 'let acc = seed;\n';
  for (let i = 0; i < childCount; i++) src += `acc = child_${i}(acc);\n`;
  src += 'return acc;\n';
  src += '}';
  return new Function(src)();
}

function makeTemplateHeavyFunction(siteCount) {
  let src = 'function tag(strs, ...args) { return strs.length + args.length; }\n';
  src += 'return function run(seed) {\n';
  src += 'let acc = 0;\n';
  for (let i = 0; i < siteCount; i++) src += `acc += tag\`${i}:\${seed}:${i}\`;\n`;
  src += 'return acc;\n';
  src += '}';
  return new Function(src)();
}

function churnGarbage(rounds, width) {
  const ring = new Array(8);
  for (let r = 0; r < rounds; r++) {
    const batch = new Array(width);
    for (let i = 0; i < width; i++) batch[i] = { i, text: 'x' + i, arr: [i, i + 1, i + 2, i + 3] };
    ring[r & 7] = batch;
  }
}

function runPlain(factory) {
  const fns = [];
  for (let i = 0; i < 20; i++) fns.push(factory());
  for (let r = 0; r < 22; r++) {
    for (let i = 0; i < fns.length; i++) fns[i](r);
    churnGarbage(72, 224);
  }
}

runPlain(() => makeChildHeavyFunction(160));
runPlain(() => makeTemplateHeavyFunction(160));

const pairs = [];
for (let i = 0; i < 20; i++) pairs.push([makeChildHeavyFunction(96), makeTemplateHeavyFunction(96)]);

let checksum = 0;
for (let r = 0; r < 22; r++) {
  for (let i = 0; i < pairs.length; i++) {
    const child = pairs[i][0](r);
    const templ = pairs[i][1](r);
    const got = child + templ;
    const expected = r + 4848;
    if (got !== expected) {
      throw new Error(`template const cache corruption at pair ${i} round ${r}: child=${child} templ=${templ} got=${got} expected=${expected}`);
    }
    checksum += got;
  }
  churnGarbage(72, 224);
}

if (checksum !== 2137740) throw new Error(`bad checksum: ${checksum}`);
console.log('ok');
