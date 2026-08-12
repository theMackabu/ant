import { Result, TaggedError, type Result as ResultType } from 'better-result';

type Product = {
  id: string;
  name: string;
  priceInCents: number;
  stock: number;
};

class ProductNotFound extends TaggedError('ProductNotFound')<{
  productId: string;
  message: string;
}> {}

class OutOfStock extends TaggedError('OutOfStock')<{
  available: number;
  productId: string;
  requested: number;
  message: string;
}> {}

class PaymentDeclined extends TaggedError('PaymentDeclined')<{
  reason: string;
  message: string;
}> {}

const products = new Map<string, Product>([['keyboard', { id: 'keyboard', name: 'Keyboard', priceInCents: 9_900, stock: 2 }]]);

const findProduct = (id: string): ResultType<Product, ProductNotFound> => {
  const product = products.get(id);
  return product ? Result.ok(product) : Result.err(new ProductNotFound({ productId: id, message: `Product ${id} was not found` }));
};

const reserve = (product: Product, quantity: number): ResultType<Product, OutOfStock> =>
  quantity <= product.stock
    ? Result.ok(product)
    : Result.err(
        new OutOfStock({
          available: product.stock,
          productId: product.id,
          requested: quantity,
          message: `Only ${product.stock} units remain`
        })
      );

const charge = (amountInCents: number): ResultType<string, PaymentDeclined> =>
  amountInCents <= 20_000
    ? Result.ok(`pay_${amountInCents}`)
    : Result.err(
        new PaymentDeclined({
          reason: 'limit_exceeded',
          message: 'The demo card has a $200 limit'
        })
      );

const checkout = (productId: string, quantity: number) =>
  Result.gen(function* () {
    if (!Number.isInteger(quantity) || quantity <= 0) {
      yield* new OutOfStock({
        available: 0,
        productId,
        requested: quantity,
        message: 'Quantity must be a positive integer'
      });
    }

    const product = yield* findProduct(productId);
    yield* reserve(product, quantity);
    const totalInCents = product.priceInCents * quantity;
    const paymentId = yield* charge(totalInCents);

    return Result.ok({ paymentId, product: product.name, quantity, totalInCents });
  });

for (const [productId, quantity] of [
  ['keyboard', 2],
  ['keyboard', 3],
  ['missing', 1]
] as const) {
  const message = checkout(productId, quantity).match({
    ok: order => `✓ ${order.quantity} × ${order.product}: $${(order.totalInCents / 100).toFixed(2)}`,
    err: error =>
      error.match({
        ProductNotFound: missing => `✗ Unknown product: ${missing.productId}`,
        OutOfStock: stock => `✗ Cannot reserve ${stock.requested}; ${stock.available} available`,
        PaymentDeclined: declined => `✗ Payment declined: ${declined.reason}`
      })
  });

  console.log(message);
}
