export const freedSet = new Set<number>();

const _mem: Record<number, unknown> = {};

function fmtPtr(n: number): string {
  return '0x' + (0x100000000 + n).toString(16);
}

export function die(addr: number, msg: string): never {
  const bin = process.argv[1]?.split('/').pop() || 'a.out';
  const pid = process.pid;
  const ptr = fmtPtr(addr);

  console.error(`${bin}(${pid}): malloc: *** error for object ${ptr}: ${msg}`);
  console.error(`${bin}(${pid}): malloc: *** set a breakpoint in malloc_error_break to debug`);
  process.exit(134);
}

export const mem: Record<number, unknown> = new Proxy(_mem, {
  get(target, prop) {
    const key = Number(prop);
    if (!isNaN(key) && freedSet.has(key)) {
      if (Ant.raw.stack?.includes('_free')) return target[key];
      die(key, 'use of deallocated pointer');
    }
    return target[key];
  },
  set(target, prop, value) {
    const key = Number(prop);
    if (!isNaN(key) && freedSet.has(key)) {
      die(key, 'use of deallocated pointer');
    }
    target[key] = value;
    return true;
  },
  deleteProperty(target, prop) {
    const key = Number(prop);
    if (!isNaN(key)) delete target[key];
    return true;
  }
});

export let brk: number = 0;
export const arena = { mem: _mem, brk, freedSet };
