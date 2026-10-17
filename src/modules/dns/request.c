#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "errors.h"
#include "list.h"
#include "gc/roots.h"
#include "silver/call.h"
#include "modules/timer.h"
#include "modules/process.h"
#include "dns_internal.h"

const dns_rr_info_t dns_rr_info[DNS_RR_COUNT] = {
#define DNS_RECORD_TYPE(name, method, syscall, ares) [DNS_RR_##name] = { #name, #method, #syscall, ares },
#include "dns/record_types.h"
};

dns_request_t *dns_request_new(ant_t *js, dns_req_kind_t kind, ant_value_t callback, ant_value_t hostname) {
  dns_request_t *req = calloc(1, sizeof(*req));
  if (!req) return NULL;

  req->js = js;
  req->kind = kind;
  req->callback = callback;
  req->hostname = hostname;
  req->owner = js_mkundef();
  req->promise = is_callable(callback) ? js_mkundef() : js_mkpromise(js);
  req->issuing = true;

  Ant_List_Insert(&js->dns_requests, req);
  return req;
}

ant_value_t dns_request_result(dns_request_t *req) {
  ant_value_t result = vtype(req->promise) == kTypePromise ? req->promise : js_mkundef();
  req->issuing = false;
  return result;
}

void dns_request_free(dns_request_t *req) {
  Ant_List_Remove(req);
  free(req);
}

static void dns_request_deliver(dns_request_t *req, ant_value_t error, ant_value_t value, const ant_value_t *cb_args, int cb_argc) {
  ant_t *js = req->js;
  ant_value_t promise = req->promise;
  ant_value_t callback = req->callback;
  bool deferred = req->issuing;
  if (req->completed) *req->completed = true;

  dns_request_free(req);
  if (js->destroying) return;

  GC_ROOT_SAVE(mark, js);
  GC_ROOT_PIN(js, promise);
  GC_ROOT_PIN(js, callback);
  GC_ROOT_PIN(js, error);
  GC_ROOT_PIN(js, value);

  if (vtype(promise) == kTypePromise) {
    if (vtype(error) != kTypeUndefined) js_reject_promise(js, promise, error);
    else js_resolve_promise(js, promise, value);
    GC_ROOT_RESTORE(js, mark);
    return;
  }

  ant_value_t argv[4] = { vtype(error) != kTypeUndefined ? error : js_mknull() };
  int argc = 1;
  for (int i = 0; vtype(error) == kTypeUndefined && i < cb_argc && i < 3; i++) argv[argc++] = cb_args[i];
  for (int i = 0; i < argc; i++) GC_ROOT_PIN(js, argv[i]);

  if (deferred) queue_next_tick_with_args(js, callback, argv, argc);
  else {
    sv_vm_call(js->vm, js, callback, js_mkundef(), argv, argc, NULL, js_mkundef());
    process_report_uncaught_exception_if_pending(js);
    process_microtasks(js);
  }

  GC_ROOT_RESTORE(js, mark);
}

void dns_request_reject(dns_request_t *req, ant_value_t error) {
  dns_request_deliver(req, error, js_mkundef(), NULL, 0);
}

void dns_request_resolve(dns_request_t *req, ant_value_t value, const ant_value_t *cb_args, int cb_argc) {
  dns_request_deliver(req, js_mkundef(), value, cb_args, cb_argc);
}

ant_value_t dns_error(ant_t *js, const char *syscall, const char *code, int errno_value, bool has_errno, const char *hostname) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t props = Ant_Object_CreateBare(js);
  GC_ROOT_PIN(js, props);

  js_set(js, props, "errno", has_errno ? js_mknum((double)errno_value) : js_mkundef());
  js_set(js, props, "code", js_mkstr(js, code, strlen(code)));
  js_set(js, props, "syscall", js_mkstr(js, syscall, strlen(syscall)));
  if (hostname) js_set(js, props, "hostname", js_mkstr(js, hostname, strlen(hostname)));

  char message[512];
  if (hostname) snprintf(message, sizeof(message), "%s %s %s", syscall, code, hostname);
  else snprintf(message, sizeof(message), "%s %s", syscall, code);

  ant_value_t error = Ant_Error_CreateWithProps(js, JS_ERR_GENERIC, props, message);
  GC_ROOT_RESTORE(js, mark);
  return error;
}

static const char *dns_ares_code(int status) {
  switch (status) {
#define DNS_ERROR_CODE(name, ares) case ares: return "E" #name;
#include "dns/error_codes.h"
    default: return "EBADRESP";
  }
}

ant_value_t dns_ares_error(ant_t *js, int status, const char *syscall, const char *hostname) {
  return dns_error(js, syscall, dns_ares_code(status), 0, false, hostname);
}

ant_value_t dns_uv_error(ant_t *js, int status, const char *syscall, const char *hostname) {
  const char *code = status == UV_EAI_NONAME || status == UV_EAI_NODATA ? "ENOTFOUND" : uv_err_name(status);
  return dns_error(js, syscall, code, status, true, hostname);
}
