let pass = true;

const seen = [];
const proxy = new Proxy({ toString: Function() }, {
  get(target, key, receiver) {
    seen.push(key);
    return Reflect.get(target, key, receiver);
  }
});

void (proxy + 3);

if (seen[0] !== Symbol.toPrimitive) {
  console.log("FAIL: proxy get trap should observe Symbol.toPrimitive first");
  pass = false;
}

if (String(seen.slice(1)) !== "valueOf,toString") {
  console.log("FAIL: proxy get trap should observe valueOf then toString during ToPrimitive");
  pass = false;
}

if (pass) console.log("PASS");
