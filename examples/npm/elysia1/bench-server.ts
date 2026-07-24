import { Elysia } from "elysia";

const app = new Elysia().get("/", () => "hello");

if (typeof globalThis.Ant === "undefined") app.listen(3000);

export default app;
