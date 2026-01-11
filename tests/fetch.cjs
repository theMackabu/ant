async function test_get() {
  const { ip } = (await fetch('https://ifconfig.co/json')).json();
  console.log(ip);
}

async function test_json() {
  const test = (await fetch('https://themackabu.dev/test.json')).json();
  console.log(JSON.stringify(test));
}

async function test_post() {
  const response = await fetch('https://httpbingo.org/post', {
    method: 'POST',
    body: JSON.stringify({ runtime: 'ant' }),
    headers: {
      'Content-Type': 'application/json',
      'User-Agent': 'ant/alpha (ant)'
    }
  });

  const { json, headers } = response.json();
  console.log(`${JSON.stringify(json)}\n${JSON.stringify(headers)}`);
}

void Promise.all([test_get(), test_post(), test_json()]);
