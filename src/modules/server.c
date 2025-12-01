#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "mongoose.h"
#include "modules/server.h"
#include "modules/timer.h"

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

static void http_handler(struct mg_connection *c, int ev, void *ev_data) {
  if (ev == MG_EV_HTTP_MSG) {
    http_server_t *server = (http_server_t *)c->fn_data;
    if (server == NULL || server->js == NULL) {
      mg_http_reply(c, 500, "Content-Type: text/plain\r\n", "Server error");
      return;
    }
    
    struct mg_http_message *hm = (struct mg_http_message *) ev_data;
    
    jsval_t result = js_mkundef();
    
    if (server->handler != 0 && js_type(server->handler) != JS_UNDEF) {
      jsval_t req = js_mkobj(server->js);
      js_set(server->js, req, "method", js_mkstr(server->js, hm->method.buf, hm->method.len));
      js_set(server->js, req, "uri", js_mkstr(server->js, hm->uri.buf, hm->uri.len));
      js_set(server->js, req, "query", js_mkstr(server->js, hm->query.buf, hm->query.len));
      js_set(server->js, req, "body", js_mkstr(server->js, hm->body.buf, hm->body.len));
      
      jsval_t res_obj = js_mkobj(server->js);
      jsval_t args[2] = {req, res_obj};
      
      result = js_call(server->js, server->handler, args, 2);
      result = wait_for_promise(server->js, result);
    }
    
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "Handler error: %s\n", js_str(server->js, result));
      mg_http_reply(c, 500, "Content-Type: text/plain\r\n", "Internal Server Error");
    } else if (js_type(result) == JS_STR) {
      char *response = js_getstr(server->js, result, NULL);
      mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "%s", response ? response : "");
    } else if (js_type(result) == JS_PRIV) {
      jsval_t status_val = js_get(server->js, result, "status");
      jsval_t body_val = js_get(server->js, result, "body");
      
      int status = 200;
      if (js_type(status_val) == JS_NUM) {
        status = (int)js_getnum(status_val);
      }
      
      char *body_str = "";
      if (js_type(body_val) == JS_STR) {
        body_str = js_getstr(server->js, body_val, NULL);
      }
      
      mg_http_reply(c, status, "Content-Type: text/plain\r\n", "%s", body_str ? body_str : "");
    } else {
      mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "Hello from Ant HTTP Server!");
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
