const express = require('express');

let app = express();
let port = 3000;

app.get('/', (_req, res) => {
  res.type('txt').send(`hello express!!\n\n🐜 ${Ant.version}\n`);
});

app.listen(port, () => {
  console.log(`started on http://localhost:${port}`);
});
