function flightReviver(key, value) {
  if (typeof value === 'string' && value[0] === '$' && value.length > 1) {
    return {
      ref: value,
      holderIsArray: Array.isArray(this),
      holderKeys: Object.keys(this).join(',')
    };
  }

  return value;
}

const topLevelArray = JSON.parse('["$5","$8"]', flightReviver);
console.log(`top0:${topLevelArray[0].ref}:${topLevelArray[0].holderIsArray}:${topLevelArray[0].holderKeys}`);
console.log(`top1:${topLevelArray[1].ref}:${topLevelArray[1].holderIsArray}:${topLevelArray[1].holderKeys}`);

const nested = JSON.parse('{"root":["$5","$8"]}', flightReviver);
console.log(`nested0:${nested.root[0].ref}:${nested.root[0].holderIsArray}:${nested.root[0].holderKeys}`);
console.log(`nested1:${nested.root[1].ref}:${nested.root[1].holderIsArray}:${nested.root[1].holderKeys}`);

const escaped = JSON.parse('{"literal":"$$escaped"}', flightReviver);
console.log(`escaped:${escaped.literal.ref || escaped.literal}`);
