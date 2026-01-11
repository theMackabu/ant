const fetchJson = (url, options) => fetch(url, options).then(r => r.json());

const test_get = async () => {
  const { ip } = await fetchJson('https://ifconfig.co/json');
  console.log(ip);
};

const test_json = async () => {
  const test = await fetchJson('https://themackabu.dev/test.json');
  console.log(JSON.stringify(test));
};

const test_post = async () => {
  const { json, headers } = await fetchJson('https://httpbingo.org/post', {
    method: 'POST',
    body: JSON.stringify({ runtime: 'ant' }),
    headers: {
      'Content-Type': 'application/json',
      'User-Agent': 'ant/alpha (ant)'
    }
  });
  console.log(`${JSON.stringify(json)}\n${JSON.stringify(headers)}`);
};

Promise.all([test_get(), test_post(), test_json()]);
