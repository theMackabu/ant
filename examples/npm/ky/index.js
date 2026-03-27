import ky from 'ky';

const json = await ky
  .post('https://httpbingo.org/post', {
    json: { kitty: '🐱' }
  })
  .json();

console.log(json);
