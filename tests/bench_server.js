function server() {
  return new Response('meow');
}

export default { fetch: server };
