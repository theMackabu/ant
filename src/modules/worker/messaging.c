#include <compat.h> // IWYU pragma: keep

#include <stdlib.h>
#include <string.h>
#include <uthash.h>

#include "ant.h"
#include "errors.h"
#include "internal.h"
#include "interrupt.h"
#include "list.h"

#include "modules/process.h"
#include "modules/timer.h"
#include "modules/worker_threads.h"

#include "worker_internal.h"

typedef enum {
  THREAD_MSG_REQUEST,
  THREAD_MSG_REPLY,
} thread_msg_kind_t;

typedef enum {
  THREAD_MSG_DELIVERED,
  THREAD_MSG_FAILED,
  THREAD_MSG_ERRORED,
} thread_msg_status_t;

typedef struct thread_msg {
  struct thread_msg *next;
  thread_msg_kind_t kind;
  thread_msg_status_t status;
  uint32_t request_id;
  uint64_t source;
  ant_thread_mailbox_t *reply_to;
  ant_wire_t *wire;
} thread_msg_t;

typedef struct thread_request {
  struct thread_request *next;
  struct thread_request **pprev;
  ant_thread_mailbox_t *mailbox;
  uint32_t id;
  ant_value_t promise;
  uv_timer_t timer;
  bool has_timer;
} thread_request_t;

struct ant_thread_mailbox {
  _Atomic int refs;
  ant_mutex_t lock;
  uint64_t thread_id;
  ant_t *js;
  bool closed;
  thread_msg_t *head;
  thread_msg_t *tail;
  uv_async_t async;
  thread_request_t *requests;
  uint32_t next_request_id;
  uint32_t request_count;
  UT_hash_handle hh;
};

static ant_mutex_t mailbox_registry_lock = ANT_MUTEX_INIT;
static ant_thread_mailbox_t *mailbox_registry = NULL;

static ant_thread_mailbox_t *mailbox_retain(ant_thread_mailbox_t *mb) {
  atomic_fetch_add_explicit(&mb->refs, 1, memory_order_relaxed);
  return mb;
}

static void thread_msg_free(thread_msg_t *msg);

static void mailbox_release(ant_thread_mailbox_t *mb) {
  if (atomic_fetch_sub_explicit(&mb->refs, 1, memory_order_acq_rel) != 1) return;
  while (mb->head) {
    thread_msg_t *msg = mb->head;
    mb->head = msg->next;
    thread_msg_free(msg);
  }
  free(mb);
}

static void thread_msg_free(thread_msg_t *msg) {
  if (msg->reply_to) mailbox_release(msg->reply_to);
  ant_wire_release(msg->wire);
  free(msg);
}

static bool mailbox_push(ant_thread_mailbox_t *mb, thread_msg_t *msg) {
  ant_mutex_lock(&mb->lock);
  bool open = !mb->closed;
  if (open) {
    msg->next = NULL;
    if (mb->tail) mb->tail->next = msg;
    else mb->head = msg;
    mb->tail = msg;
    uv_async_send(&mb->async);
  }
  ant_mutex_unlock(&mb->lock);
  return open;
}

static void mailbox_reply(ant_thread_mailbox_t *to, uint32_t id, thread_msg_status_t status) {
  thread_msg_t *msg = calloc(1, sizeof(*msg));
  if (!msg) return;
  msg->kind = THREAD_MSG_REPLY;
  msg->request_id = id;
  msg->status = status;
  if (!mailbox_push(to, msg)) free(msg);
}

static ant_value_t mailbox_error(ant_t *js, thread_msg_status_t status) {
  ant_value_t err = status == THREAD_MSG_ERRORED
    ? js_mkerr_code(js, JS_ERR_GENERIC, "ERR_WORKER_MESSAGING_ERRORED", "The destination thread threw an error while processing the message")
    : js_mkerr_code(js, JS_ERR_GENERIC, "ERR_WORKER_MESSAGING_FAILED", "Cannot find the destination thread or listener");
  return js_take_thrown(js, err);
}

static ant_value_t mailbox_rejected(ant_t *js, ant_value_t err) {
  ant_value_t promise = js_mkpromise(js);
  if (is_err(err)) err = js_take_thrown(js, err);
  js_reject_promise(js, promise, err);
  return promise;
}

static void thread_request_timer_closed(uv_handle_t *handle) {
  free(handle->data);
}

static void thread_request_free(thread_request_t *req) {
  ant_thread_mailbox_t *mb = req->mailbox;
  Ant_List_Remove(req);
  req->promise = js_mkundef();
  if (--mb->request_count == 0) uv_unref((uv_handle_t *)&mb->async);
  if (!req->has_timer) {
    free(req);
    return;
  }
  uv_timer_stop(&req->timer);
  uv_close((uv_handle_t *)&req->timer, thread_request_timer_closed);
}

static void thread_request_settle(ant_t *js, thread_request_t *req, ant_value_t err) {
  GC_ROOT_SAVE(mark, js);
  ant_value_t promise = req->promise;
  GC_ROOT_PIN(js, promise);
  thread_request_free(req);
  if (vtype(err) == kTypeUndefined) js_resolve_promise(js, promise, err);
  else {
    GC_ROOT_PIN(js, err);
    js_reject_promise(js, promise, err);
  }
  GC_ROOT_RESTORE(js, mark);
}

static void thread_request_timeout(uv_timer_t *timer) {
  thread_request_t *req = timer->data;
  ant_t *js = req->mailbox->js;
  thread_request_settle(js, req, js_take_thrown(js, js_mkerr_code(
    js, JS_ERR_GENERIC, "ERR_WORKER_MESSAGING_TIMEOUT", "Sending a message to another thread timed out"
  )));
  process_microtasks(js);
}

static thread_msg_status_t mailbox_deliver(ant_t *js, thread_msg_t *msg) {
  if (!process_has_event_listeners(js, "workerMessage")) return THREAD_MSG_FAILED;
  
  ant_value_t value = ant_wire_deserialize(js, msg->wire, NULL);
  if (is_err(value)) {
    js_take_thrown(js, value);
    return THREAD_MSG_ERRORED;
  }

  GC_ROOT_SAVE(mark, js);
  ant_value_t argv[2] = { value, js_mknum((double)msg->source) };
  
  GC_ROOT_PIN(js, argv[0]);
  emit_process_event(js, "workerMessage", argv, 2);
  GC_ROOT_RESTORE(js, mark);
  
  if (!Ant_Exception_Pending(js)) return THREAD_MSG_DELIVERED;
  if (js_termination_pending(js)) return THREAD_MSG_FAILED;
  js_take_thrown(js, js_mkundef());
  
  return THREAD_MSG_ERRORED;
}

static void mailbox_wake(uv_async_t *handle) {
  ant_thread_mailbox_t *mb = handle->data;
  ant_t *js = mb->js;

  while (!js->destroying && !Ant_Exception_Pending(js)) {
    ant_mutex_lock(&mb->lock);
    thread_msg_t *msg = mb->head;
    if (msg) {
      mb->head = msg->next;
      if (!mb->head) mb->tail = NULL;
    }
    ant_mutex_unlock(&mb->lock);
    if (!msg) break;

    if (msg->kind == THREAD_MSG_REQUEST)
      mailbox_reply(msg->reply_to, msg->request_id, mailbox_deliver(js, msg));
    else {
      thread_request_t *req = mb->requests;
      while (req && req->id != msg->request_id) req = req->next;
      if (req) thread_request_settle(js, req, msg->status == THREAD_MSG_DELIVERED
        ? js_mkundef() : mailbox_error(js, msg->status));
    }
    thread_msg_free(msg);
    process_microtasks(js);
  }
}

static ant_thread_mailbox_t *worker_mailbox(ant_t *js) {
  if (js->thread_mailbox) return js->thread_mailbox;
  ant_thread_mailbox_t *mb = calloc(1, sizeof(*mb));
  if (!mb) return NULL;
  if (uv_async_init(js->loop, &mb->async, mailbox_wake) != 0) {
    free(mb);
    return NULL;
  }
  mb->async.data = mb;
  uv_unref((uv_handle_t *)&mb->async);
  atomic_init(&mb->refs, 1);
  mb->lock = (ant_mutex_t)ANT_MUTEX_INIT;
  mb->thread_id = ant_worker_thread_id(js);
  mb->js = js;
  js->thread_mailbox = mb;

  ant_mutex_lock(&mailbox_registry_lock);
  HASH_ADD(hh, mailbox_registry, thread_id, sizeof(mb->thread_id), mb);
  ant_mutex_unlock(&mailbox_registry_lock);
  return mb;
}

void ant_worker_messaging_listen(ant_t *js) {
  if (!js->destroying) worker_mailbox(js);
}

static void mailbox_async_closed(uv_handle_t *handle) {
  mailbox_release(handle->data);
}

void worker_messaging_close(ant_t *js) {
  ant_thread_mailbox_t *mb = js->thread_mailbox;
  if (!mb) return;
  js->thread_mailbox = NULL;

  ant_mutex_lock(&mailbox_registry_lock);
  HASH_DELETE(hh, mailbox_registry, mb);
  ant_mutex_unlock(&mailbox_registry_lock);

  ant_mutex_lock(&mb->lock);
  mb->closed = true;
  thread_msg_t *msg = mb->head;
  mb->head = mb->tail = NULL;
  ant_mutex_unlock(&mb->lock);

  while (msg) {
    thread_msg_t *next = msg->next;
    if (msg->kind == THREAD_MSG_REQUEST) mailbox_reply(msg->reply_to, msg->request_id, THREAD_MSG_FAILED);
    thread_msg_free(msg);
    msg = next;
  }

  while (mb->requests) thread_request_free(mb->requests);
  uv_close((uv_handle_t *)&mb->async, mailbox_async_closed);
}

static ant_thread_mailbox_t *mailbox_find(uint64_t thread_id) {
  ant_thread_mailbox_t *mb = NULL;
  ant_mutex_lock(&mailbox_registry_lock);
  HASH_FIND(hh, mailbox_registry, &thread_id, sizeof(thread_id), mb);
  if (mb) mailbox_retain(mb);
  ant_mutex_unlock(&mailbox_registry_lock);
  return mb;
}

ant_value_t worker_post_message_to_thread(ant_native_params_t) {
  ant_value_t id = nargs > 0 ? args[0] : js_mkundef();
  ant_value_t value = nargs > 1 ? args[1] : js_mkundef();
  ant_value_t transfer = nargs > 2 ? args[2] : js_mkundef();
  ant_value_t timeout = nargs > 3 ? args[3] : js_mkundef();

  if (vtype(transfer) == kTypeNumber && vtype(timeout) == kTypeUndefined) {
    timeout = transfer;
    transfer = js_mkundef();
  }
  
  if (vtype(timeout) != kTypeUndefined) {
    if (vtype(timeout) != kTypeNumber)
      return mailbox_rejected(js, Ant_Error_CreateInvalidArgType(js, "timeout", "of type number", timeout));
    if (!(js_getnum(timeout) >= 0))
      return mailbox_rejected(js, Ant_Error_CreateOutOfRange(js, "timeout", ">= 0", timeout));
  }

  uint64_t self = ant_worker_thread_id(js);
  double num = vtype(id) == kTypeNumber ? js_getnum(id) : -1;
  
  if (num == (double)self) return mailbox_rejected(js, js_mkerr_code(
    js, JS_ERR_GENERIC, 
    "ERR_WORKER_MESSAGING_SAME_THREAD", 
    "Cannot sent a message to the same thread"
  ));

  ant_thread_mailbox_t *own = worker_mailbox(js);
  if (!own) return mailbox_rejected(js, Ant_Error_CreateOutOfMemory(js));

  ant_wire_t *wire = NULL;
  ant_value_t ser = ant_wire_serialize(js, value, transfer, js_mkundef(), &wire);
  if (is_err(ser)) return mailbox_rejected(js, ser);

  bool valid = num >= 0 && num <= 9007199254740991.0 && num == (double)(uint64_t)num;
  ant_thread_mailbox_t *dest = valid ? mailbox_find((uint64_t)num) : NULL;
  
  if (!dest) {
    ant_wire_release(wire);
    return mailbox_rejected(js, mailbox_error(js, THREAD_MSG_FAILED));
  }

  thread_request_t *req = calloc(1, sizeof(*req));
  thread_msg_t *msg = calloc(1, sizeof(*msg));
  
  if (!req || !msg) {
    free(req);
    free(msg);
    ant_wire_release(wire);
    mailbox_release(dest);
    return mailbox_rejected(js, Ant_Error_CreateOutOfMemory(js));
  }

  ant_value_t promise = js_mkpromise(js);
  req->mailbox = own;
  req->id = ++own->next_request_id;
  req->promise = promise;
  
  Ant_List_Insert(&own->requests, req);
  if (own->request_count++ == 0) uv_ref((uv_handle_t *)&own->async);

  if (vtype(timeout) == kTypeNumber && uv_timer_init(js->loop, &req->timer) == 0) {
    req->has_timer = true;
    req->timer.data = req;
    uv_timer_start(&req->timer, thread_request_timeout, (uint64_t)js_getnum(timeout), 0);
  }

  msg->kind = THREAD_MSG_REQUEST;
  msg->request_id = req->id;
  msg->source = self;
  msg->reply_to = mailbox_retain(own);
  msg->wire = wire;
  bool sent = mailbox_push(dest, msg);
  
  mailbox_release(dest);
  if (!sent) {
    thread_msg_free(msg);
    thread_request_settle(js, req, mailbox_error(js, THREAD_MSG_FAILED));
  }
  
  return promise;
}

void worker_messaging_gc_mark(ant_t *js, gc_mark_fn mark) {
  if (!js->thread_mailbox) return;
  for (thread_request_t *req = js->thread_mailbox->requests; req; req = req->next)
    if (vtype(req->promise) == kTypePromise) mark(js, req->promise);
}
