const enabled = process.stdout.isTTY && !process.env.NO_COLOR;

function paint(code, text) {
  return enabled ? `\x1b[${code}m${text}\x1b[0m` : text;
}

module.exports = {
  green: text => paint(32, text),
  dim: text => paint(2, text),
  bold: text => paint(1, text)
};
