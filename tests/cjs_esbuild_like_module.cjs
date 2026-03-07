function build(options) {
  return { ok: true, options: options ?? null };
}

function transform(source) {
  return String(source).toUpperCase();
}

module.exports = {
  build,
  transform,
  version: "0.0-test",
};
