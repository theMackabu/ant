let events = [];

Promise.resolve().then(() => {
  events.push("outer");
  Promise.resolve().then(() => {
    events.push("inner");
  });
});

setTimeout(() => {
  events.push("timer");
  console.log(events.join(","));

  if (events.join(",") === "outer,inner,timer") {
    console.log("OK nested microtasks drain before timers");
  } else {
    console.log("FAIL nested microtasks order changed");
  }
}, 0);
