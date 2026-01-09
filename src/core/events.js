function createEventTarget() {
  const obj = {};
  obj.addEventListener = EventTargetPrototype.addEventListener;
  obj.removeEventListener = EventTargetPrototype.removeEventListener;
  obj.dispatchEvent = EventTargetPrototype.dispatchEvent;
  return obj;
}
