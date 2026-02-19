const jq = "jquery";
const foo = 42;
const bar = "hello";
const baz = true;

export { jq as "matrix" };
export { foo as "foo-bar" };
export { bar as "unicode \u0041" };
export { baz as "default" };
