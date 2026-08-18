export default {
  async scheduled(controller) {
    await new Promise(resolve => setTimeout(resolve, 5));
    if (process.env.ANT_CRON_REJECT) throw new Error('scheduled rejection');
    console.log(JSON.stringify(controller));
  },
};
