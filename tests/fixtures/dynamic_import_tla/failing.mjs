await new Promise((resolve) => setTimeout(resolve, 2));
throw new RangeError('dependency failed');
