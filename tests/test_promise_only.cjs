console.log("Start");

new Promise((resolve) => {
  setTimeout(() => {
    console.log("Timer fired");
    resolve("Done");
  }, 100);
}).then(result => {
  console.log("Promise resolved:", result);
});

console.log("End");
