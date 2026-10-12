import { v5 as uuidv5 } from 'https://esm.sh/uuid';

const id = uuidv5('https://example.com/article/42', uuidv5.URL);
const id2 = uuidv5('https://example.com/article/42', uuidv5.URL);
console.log(id === id2)

const MY_APP = uuidv5('myapp.example.com', uuidv5.DNS);
const userId = uuidv5('user:42', MY_APP);
console.log(userId)