#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "mongoose.h"
#include "modules/server.h"
#include "modules/timer.h"
#include "ant.h"

typedef struct {
  int status;
  char *body;
  char *content_type;
  int sent;
} response_ctx_t;

typedef struct {
  struct js *js;
  jsval_t handler;
  int port;
} http_server_t;

static struct mg_mgr s_mgr;
static int g_mgr_initialized = 0;

static void server_signal_handler(int signum) {
  (void)signum;
  printf("\nShutting down server...\n");
  
  if (g_mgr_initialized) {
    mg_mgr_free(&s_mgr);
    g_mgr_initialized = 0;
  }
  
  exit(0);
}

static jsval_t wait_for_promise(struct js *js, jsval_t promise_val) {
  if (js_type(promise_val) != JS_PRIV) return promise_val;
  
  jsval_t state_check = js_get(js, promise_val, "__state");
  if (js_type(state_check) == JS_UNDEF) return promise_val;
  
  while (has_pending_microtasks()) {
    process_microtasks(js);
    
    jsval_t state_val = js_get(js, promise_val, "__state");
    if (js_type(state_val) == JS_NUM) {
      int state = (int)js_getnum(state_val);
      if (state != 0) {
        jsval_t value_val = js_get(js, promise_val, "__value");
        return value_val;
      }
    }
  }
  
  jsval_t state_val = js_get(js, promise_val, "__state");
  if (js_type(state_val) == JS_NUM) {
    int state = (int)js_getnum(state_val);
    if (state != 0) {
      jsval_t value_val = js_get(js, promise_val, "__value");
      return value_val;
    }
  }
  
  return promise_val;
}

static jsval_t res_status(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  if (js_type(args[0]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[0]);
  }
  
  return js_mkundef();
}

static jsval_t res_body(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  if (js_type(args[0]) == JS_STR) {
    ctx->body = js_getstr(js, args[0], NULL);
  }
  
  if (nargs >= 2 && js_type(args[1]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[1]);
  }
  
  if (nargs >= 3 && js_type(args[2]) == JS_STR) {
    ctx->content_type = js_getstr(js, args[2], NULL);
  } else {
    ctx->content_type = "text/plain";
  }
  
  ctx->sent = 1;
  
  return js_mkundef();
}

static jsval_t res_html(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  if (js_type(args[0]) == JS_STR) {
    ctx->body = js_getstr(js, args[0], NULL);
  }
  
  if (nargs >= 2 && js_type(args[1]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[1]);
  }
  
  ctx->content_type = "text/html";
  ctx->sent = 1;
  
  return js_mkundef();
}

static jsval_t res_json(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkundef();
  
  jsval_t this_val = js_getthis(js);
  jsval_t ctx_val = js_get(js, this_val, "__response_ctx");
  if (js_type(ctx_val) != JS_NUM) return js_mkundef();
  
  response_ctx_t *ctx = (response_ctx_t *)(unsigned long)js_getnum(ctx_val);
  if (!ctx) return js_mkundef();
  
  const char *json_str = js_str(js, args[0]);
  if (json_str) {
    ctx->body = (char *)json_str;
  }
  
  if (nargs >= 2 && js_type(args[1]) == JS_NUM) {
    ctx->status = (int)js_getnum(args[1]);
  }
  
  ctx->content_type = "application/json";
  ctx->sent = 1;
  
  return js_mkundef();
}

static void http_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    http_server_t *server = (http_server_t *)c->fn_data;
    if (server == NULL || server->js == NULL) {
      mg_http_reply(c, 500, "Content-Type: text/plain\r\n", "Server error");
      return;
    }
    
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    
    jsval_t result = js_mkundef();
    
    response_ctx_t res_ctx = {
      .status = 200,
      .body = "",
      .content_type = "text/plain",
      .sent = 0
    };
    
    if (server->handler != 0 && js_type(server->handler) != JS_UNDEF) {
      jsval_t req = js_mkobj(server->js);
      js_set(server->js, req, "method", js_mkstr(server->js, hm->method.buf, hm->method.len));
      js_set(server->js, req, "uri", js_mkstr(server->js, hm->uri.buf, hm->uri.len));
      js_set(server->js, req, "query", js_mkstr(server->js, hm->query.buf, hm->query.len));
      js_set(server->js, req, "body", js_mkstr(server->js, hm->body.buf, hm->body.len));
      
      jsval_t res_obj = js_mkobj(server->js);
      js_set(server->js, res_obj, "__response_ctx", js_mknum((unsigned long)&res_ctx));
      js_set(server->js, res_obj, "status", js_mkfun(res_status));
      js_set(server->js, res_obj, "body", js_mkfun(res_body));
      js_set(server->js, res_obj, "html", js_mkfun(res_html));
      js_set(server->js, res_obj, "json", js_mkfun(res_json));
      
      jsval_t args[2] = {req, res_obj};
      
      result = js_call(server->js, server->handler, args, 2);
      result = wait_for_promise(server->js, result);
      
      if (res_ctx.sent) {
        char headers[256];
        snprintf(headers, sizeof(headers), "Content-Type: %s\r\n", 
                 res_ctx.content_type ? res_ctx.content_type : "text/plain");
        mg_http_reply(c, res_ctx.status, headers, "%s", res_ctx.body ? res_ctx.body : "");
        return;
      }
    }
    
    // If we get here, no response was sent
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "Handler error: %s\n", js_str(server->js, result));
      mg_http_reply(c, 500, "Content-Type: text/plain\r\n", "Internal Server Error");
    } else {
      mg_http_reply(c, 404, "Content-Type: text/plain\r\n", "Not Found");
    }
  }
}

jsval_t js_serve(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    fprintf(stderr, "Error: Ant.serve() requires at least 1 argument (port)\n");
    return js_mkundef();
  }
  
  int port = 8000;
  if (js_type(args[0]) == JS_NUM) {
    port = (int)js_getnum(args[0]);
  }
  
  http_server_t *server = malloc(sizeof(http_server_t));
  if (server == NULL) {
    fprintf(stderr, "Error: Failed to allocate server data\n");
    return js_mkundef();
  }
  
  server->js = js;
  server->port = port;
  server->handler = (nargs >= 2) ? args[1] : js_mkundef();
  
  char url[64];
  snprintf(url, sizeof(url), "http://0.0.0.0:%d", port);
  
  if (!g_mgr_initialized) {
    mg_log_set(MG_LL_INFO);
    mg_mgr_init(&s_mgr);
    g_mgr_initialized = 1;
    
    signal(SIGINT, server_signal_handler);
    signal(SIGTERM, server_signal_handler);
  }
  
  struct mg_connection *c = mg_http_listen(&s_mgr, url, http_handler, server);
  if (c == NULL) {
    fprintf(stderr, "Error: Failed to start HTTP server on port %d\n", port);
    free(server);
    return js_mknum(0);
  }
    
  for (;;) {
    mg_mgr_poll(&s_mgr, 1000);
  }
  
  return js_mknum(1);
}
