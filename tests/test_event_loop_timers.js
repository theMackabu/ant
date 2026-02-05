let fired = false;

setTimeout(() => {
  fired = true;
  console.log('timer fired');
}, 5);

// If the event loop exits early, this will print false.
setTimeout(() => {
  if (!fired) {
    throw new Error('timer status: missing');
  }
  console.log('timer status: ok');
}, 10);
