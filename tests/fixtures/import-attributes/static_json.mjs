import data from './data.json' with { type: 'json' };
import again from './data.json' with { 'type': 'json', };
import './data.json' with { type: 'json' };
import { value } from './plain.mjs' with {};
import spaced from './data.json'
  with { type: 'json' };

export default { data, same: data === again && data === spaced, value };
