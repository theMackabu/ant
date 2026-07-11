const assert = (condition, message) => {
  if (!condition) throw new Error(message);
};

(function () {
  "use strict";

  const sloppy = Function("return this");
  assert(sloppy() === globalThis,
    "Function constructor must not inherit caller strictness");

  const strict = Function('"use strict"; return this');
  assert(strict() === undefined,
    "Function constructor must honor its own use strict directive");
})();

console.log("Function constructor strictness tests passed");
