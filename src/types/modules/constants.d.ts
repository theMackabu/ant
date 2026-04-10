declare module 'constants' {
  const constants: Record<string, number>;
  export = constants;
}

declare module 'ant:constants' {
  export * from 'constants';
}

declare module 'node:constants' {
  export * from 'constants';
}
