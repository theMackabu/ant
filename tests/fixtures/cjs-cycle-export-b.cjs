const a = require('./cjs-cycle-export-a.cjs');

module.exports = {
  seenType: typeof a.applyToClass,
  seenValue: a.applyToClass(),
};
