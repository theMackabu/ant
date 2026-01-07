function meow(c) {}

function server(c) {
  c.res.body('meow');
}

Ant.serve(8000, server);
