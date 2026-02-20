// Monad-style bind chain (simplified newt pattern)
const MkM = h1 => ({ tag: 0, h1: h1 });

const bind = (m, f) =>
  MkM(tc => eta => {
    const sc = m.h1(tc)(eta);
    if (sc.tag === 1) return f(sc.h2.h3).h1(sc.h2.h2)(eta);
    return sc;
  });

const pure = v => MkM(tc => eta => ({ tag: 1, h2: { h2: tc, h3: v } }));

let chain = pure(0);
for (let i = 0; i < 800; i++) {
  chain = bind(chain, v => pure(v + 1));
}
const result = chain.h1({})(0);
console.log('monad chain result:', result.h2.h3); // 100
