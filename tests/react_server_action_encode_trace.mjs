const knownServerReferences = new WeakMap();
const boundCache = new WeakMap();
const FunctionBind = Function.prototype.bind;
const ArraySlice = Array.prototype.slice;

function encodeFormData(reference) {
  let resolve;
  let reject;
  const thenable = new Promise((res, rej) => {
    resolve = res;
    reject = rej;
  });

  Promise.resolve(reference.bound).then((bound) => {
    const data = new FormData();
    data.append('id', reference.id);
    data.append('boundCount', String(bound.length));
    thenable.status = 'fulfilled';
    thenable.value = data;
    resolve(data);
  }, (error) => {
    thenable.status = 'rejected';
    thenable.reason = error;
    reject(error);
  });

  return thenable;
}

function defaultEncodeFormAction(identifierPrefix) {
  let referenceClosure = knownServerReferences.get(this);
  let data = null;

  if (referenceClosure.bound !== null) {
    data = boundCache.get(referenceClosure);
    if (!data) {
      data = encodeFormData({
        id: referenceClosure.id,
        bound: referenceClosure.bound,
      });
      boundCache.set(referenceClosure, data);
    }

    if (data.status === 'rejected') throw data.reason;
    if (data.status !== 'fulfilled') throw data;

    referenceClosure = data.value;
    const prefixedData = new FormData();
    referenceClosure.forEach((value, key) => {
      prefixedData.append(`$ACTION_${identifierPrefix}:${key}`, value);
    });
    data = prefixedData;
    referenceClosure = `$ACTION_REF_${identifierPrefix}`;
  } else {
    referenceClosure = `$ACTION_ID_${referenceClosure.id}`;
  }

  return {
    name: referenceClosure,
    method: 'POST',
    encType: 'multipart/form-data',
    data,
  };
}

function bind() {
  const referenceClosure = knownServerReferences.get(this);
  if (!referenceClosure) return FunctionBind.apply(this, arguments);

  const newFn = referenceClosure.originalBind.apply(this, arguments);
  const args = ArraySlice.call(arguments, 1);
  const boundPromise = referenceClosure.bound !== null
    ? Promise.resolve(referenceClosure.bound).then((boundArgs) => boundArgs.concat(args))
    : Promise.resolve(args);

  knownServerReferences.set(newFn, {
    id: referenceClosure.id,
    originalBind: newFn.bind,
    bound: boundPromise,
  });

  Object.defineProperties(newFn, {
    $$FORM_ACTION: { value: this.$$FORM_ACTION },
    bind: { value: bind },
  });

  return newFn;
}

function registerBoundServerReference(reference, id, bound) {
  knownServerReferences.set(reference, {
    id,
    originalBind: reference.bind,
    bound,
  });

  Object.defineProperties(reference, {
    $$FORM_ACTION: { value: defaultEncodeFormAction },
    bind: { value: bind },
  });
}

async function action(x) {
  return x;
}

registerBoundServerReference(action, 'demo', null);

const bound = action.bind(null, 1);
console.log(`bound.formAction.type:${typeof bound.$$FORM_ACTION}`);

try {
  const info = bound.$$FORM_ACTION('demo');
  console.log(`first.name:${info.name}`);
} catch (error) {
  console.log(`first.throw.type:${typeof error}`);
  console.log(`first.throw.then:${typeof error?.then}`);
  console.log(`first.throw.status:${error?.status ?? 'missing'}`);
}

await Promise.resolve();

try {
  const info = bound.$$FORM_ACTION('demo');
  console.log(`second.name:${info.name}`);
  console.log(`second.method:${info.method}`);
  console.log(`second.encType:${info.encType}`);
  const pairs = [];
  info.data?.forEach((value, key) => {
    pairs.push(`${key}=${value}`);
  });
  console.log(`second.data:${pairs.join(',')}`);
} catch (error) {
  console.log(`second.throw:${error?.message ?? String(error)}`);
}
