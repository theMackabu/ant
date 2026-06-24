if (window !== globalThis) {
  throw new Error("global window should resolve to globalThis");
}

{
  const window = new WeakMap;
  if (!(window instanceof WeakMap)) {
    throw new Error("local window binding should shadow global window");
  }
  if (typeof window.has !== "function") {
    throw new Error(`local window WeakMap should expose has(), got ${typeof window.has}`);
  }
}

console.log("test_window_identifier_shadowing: OK");

