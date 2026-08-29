Reflect.set(process.env, 'NODE_ENV', 'production');
export default (await import('./hello.js')).default;
