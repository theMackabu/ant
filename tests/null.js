const NullProtoObj = (() => {
  const e = function () {};
  return ((e.prototype = Object.create(null)), Object.freeze(e.prototype), e);
})();

console.log(new NullProtoObj());
