import { Ant } from '../dist/index.js';

const order = {
  customer: { tier: 'gold' },
  items: [
    { price: 35, quantity: 2 },
    { price: 20, quantity: 3 }
  ]
};

const rules = {
  subtotal: `
    order.items.reduce(
      (total, item) => total + item.price * item.quantity,
      0,
    )
  `,
  discount: `
    order.customer.tier === "gold" &&
    order.items.reduce(
      (total, item) => total + item.price * item.quantity,
      0,
    ) >= 100
      ? 0.15
      : 0
  `
};

async function evaluateRule(source, input) {
  const ant = await Ant.create({
    memoryLimit: 32 * 1024 * 1024,
    timeout: 100,
    globals: { order: input }
  });

  try {
    return await ant.eval(source);
  } finally {
    ant.dispose();
  }
}

for (const [name, source] of Object.entries(rules)) {
  console.log(name, await evaluateRule(source, order));
}
