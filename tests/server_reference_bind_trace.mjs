const knownServerReferences = new WeakMap();
const FunctionBind = Function.prototype.bind;
const ArraySlice = Array.prototype.slice;

function defaultEncodeFormAction() {
  return { name: '$ACTION_ID_demo', method: 'POST' };
}

function registerBoundServerReference(reference, id) {
  if (knownServerReferences.has(reference)) return;

  knownServerReferences.set(reference, {
    id,
    originalBind: reference.bind,
    bound: null,
  });

  Object.defineProperties(reference, {
    $$FORM_ACTION: {
      value: defaultEncodeFormAction,
    },
    bind: {
      value: bind,
    },
  });
}

function bind() {
  const referenceClosure = knownServerReferences.get(this);
  if (!referenceClosure) return FunctionBind.apply(this, arguments);

  const newFn = referenceClosure.originalBind.apply(this, arguments);
  const args = ArraySlice.call(arguments, 1);

  knownServerReferences.set(newFn, {
    id: referenceClosure.id,
    originalBind: newFn.bind,
    bound: Promise.resolve(args),
  });

  Object.defineProperties(newFn, {
    $$FORM_ACTION: {
      value: this.$$FORM_ACTION,
    },
    bind: {
      value: bind,
    },
  });

  return newFn;
}

async function action(x) {
  return x;
}

registerBoundServerReference(action, 'demo');

console.log(`action.bind.eq.custom:${action.bind === bind}`);
console.log(`action.formAction.type:${typeof action.$$FORM_ACTION}`);

const bound = action.bind(null, 1);

console.log(`bound.type:${typeof bound}`);
console.log(`bound.bind.eq.custom:${bound.bind === bind}`);
console.log(`bound.formAction.type:${typeof bound.$$FORM_ACTION}`);
console.log(`bound.formAction.name:${bound.$$FORM_ACTION ? bound.$$FORM_ACTION().name : 'missing'}`);
