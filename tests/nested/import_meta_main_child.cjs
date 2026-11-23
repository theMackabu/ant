module.exports = {
  main: import.meta.main,
  filename: import.meta.filename,
  readMain: () => import.meta.main,
};
