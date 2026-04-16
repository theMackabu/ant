function snapshotAssignmentAlias() {
  const f = fetch;
  const obj = {};

  obj.a = f;
  obj.b = f;

  return {
    fetchType: typeof fetch,
    fetchName: fetch.name,
    sameStoredValue: obj.a === obj.b,
    sameAsFetchA: obj.a === fetch,
    sameAsFetchB: obj.b === fetch,
    storedNames: {
      a: obj.a && obj.a.name,
      b: obj.b && obj.b.name,
    },
  };
}

function snapshotPromotedState() {
  const sym = Symbol('native-fn-repro');
  const originalProto = Object.getPrototypeOf(fetch);
  const customProto = { fromCustomProto: 42 };

  fetch.extra = 'value-from-set';
  Object.defineProperty(fetch, 'defined', {
    value: 'value-from-defineProperty',
    enumerable: true,
    configurable: true,
    writable: true,
  });
  Object.defineProperty(fetch, sym, {
    value: 'symbol-value',
    enumerable: true,
    configurable: true,
    writable: true,
  });
  Object.setPrototypeOf(fetch, customProto);

  const names = Object.getOwnPropertyNames(fetch);
  const symbols = Object.getOwnPropertySymbols(fetch);
  const descExtra = Object.getOwnPropertyDescriptor(fetch, 'extra');
  const descDefined = Object.getOwnPropertyDescriptor(fetch, 'defined');
  const descSym = Object.getOwnPropertyDescriptor(fetch, sym);

  const snapshot = {
    extraRead: fetch.extra,
    definedRead: fetch.defined,
    protoWasUpdated: Object.getPrototypeOf(fetch) === customProto,
    inheritedRead: fetch.fromCustomProto,
    ownNames: names,
    ownSymbols: symbols.map((entry) => String(entry)),
    hasExtraName: names.includes('extra'),
    hasDefinedName: names.includes('defined'),
    hasSymbol: symbols.includes(sym),
    extraDescriptor: descExtra && {
      value: descExtra.value,
      enumerable: descExtra.enumerable,
      configurable: descExtra.configurable,
      writable: descExtra.writable,
    },
    definedDescriptor: descDefined && {
      value: descDefined.value,
      enumerable: descDefined.enumerable,
      configurable: descDefined.configurable,
      writable: descDefined.writable,
    },
    symbolDescriptor: descSym && {
      value: descSym.value,
      enumerable: descSym.enumerable,
      configurable: descSym.configurable,
      writable: descSym.writable,
    },
  };

  delete fetch.extra;
  delete fetch.defined;
  delete fetch[sym];
  Object.setPrototypeOf(fetch, originalProto);

  return snapshot;
}

const result = {
  assignmentAlias: snapshotAssignmentAlias(),
  promotedState: snapshotPromotedState(),
};

console.log(JSON.stringify(result, null, 2));
