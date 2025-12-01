async function test_get() {
  const { origin } = (await fetch('https://httpbin.org/get')).json();
  console.log(origin);
}

async function test_post() {
  const response = await fetch('https://httpbin.org/post', {
    method: 'POST',
    body: JSON.stringify({ runtime: 'ant' }),
    headers: {
      'Content-Type': 'application/json',
      'User-Agent': 'ant/alpha (ant)'
    }
  });

  const { json, headers } = response.json();
  console.log(`${json}\n${headers}`);
}

void test_get();
void test_post();
