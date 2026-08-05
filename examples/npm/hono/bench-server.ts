import { Hono } from "hono";

const app = new Hono();
app.get("/", (context) => context.text("hello"));

export default app;
