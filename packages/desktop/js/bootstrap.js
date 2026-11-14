(native, registerHooks, source) =>
  registerHooks({
    resolve(specifier, context, next) {
      if (specifier !== 'ant:desktop') return next(specifier, context);
      return { url: 'ant:desktop', format: 'module', shortCircuit: true };
    },
    load(url, context, next) {
      if (url !== 'ant:desktop') return next(url, context);
      globalThis.__antDesktopNative = native;
      return { format: 'module', source, shortCircuit: true };
    }
  })
