#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "errors.h"
#include "internal.h"
#include "runtime.h"
#include "silver/engine.h"
#include "descriptors.h"

#include "modules/symbol.h"
#include "modules/observable.h"

static inline bool is_callable(ant_value_t val) {
  uint8_t t = vtype(val);
  return t == T_FUNC || t == T_CFUNC;
}

static bool subscription_closed(ant_t *js, ant_value_t subscription) {
  ant_value_t observer = js_get_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER);
  return vtype(observer) == T_UNDEF;
}

static void cleanup_subscription(ant_t *js, ant_value_t subscription) {
  ant_value_t cleanup = js_get_slot(subscription, SLOT_SUBSCRIPTION_CLEANUP);
  if (vtype(cleanup) == T_UNDEF) return;
  if (!is_callable(cleanup)) return;
  
  js_set_slot(subscription, SLOT_SUBSCRIPTION_CLEANUP, js_mkundef());
  ant_value_t result = sv_vm_call(js->vm, js, cleanup, js_mkundef(), NULL, 0, NULL, false);
  
  if (vtype(result) == T_ERR) fprintf(stderr, "Error in subscription cleanup: %s\n", js_str(js, result));
}

static ant_value_t create_subscription(ant_t *js, ant_value_t observer) {
  ant_value_t subscription = js_mkobj(js);
  js_set_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER, observer);
  js_set_slot(subscription, SLOT_SUBSCRIPTION_CLEANUP, js_mkundef());
  js_set_sym(js, subscription, get_toStringTag_sym(), js_mkstr(js, "Subscription", 12));
  return subscription;
}

static ant_value_t js_subscription_get_closed(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t subscription = js_getthis(js);
  
  if (!is_special_object(subscription)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Subscription.closed getter called on non-object");
  }
  return js_bool(subscription_closed(js, subscription));
}

static ant_value_t js_subscription_unsubscribe(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t subscription = js_getthis(js);
  
  if (!is_special_object(subscription)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Subscription.unsubscribe called on non-object");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  js_set_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER, js_mkundef());
  cleanup_subscription(js, subscription);
  
  return js_mkundef();
}

static void setup_subscription_methods(ant_t *js, ant_value_t subscription) {
  js_set(js, subscription, "unsubscribe", js_mkfun(js_subscription_unsubscribe));
  ant_value_t closed_getter = js_mkfun(js_subscription_get_closed);
  js_set_getter_desc(js, subscription, "closed", 6, closed_getter, JS_DESC_E | JS_DESC_C);
}

static ant_value_t js_subobs_get_closed(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t O = js_getthis(js);
  
  if (!is_special_object(O)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.closed getter called on non-object");
  }
  
  ant_value_t subscription = js_get_slot(O, SLOT_DATA);
  if (!is_special_object(subscription)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  return js_bool(subscription_closed(js, subscription));
}

static ant_value_t js_subobs_next(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t O = js_getthis(js);
  
  if (!is_special_object(O)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.next called on non-object");
  }
  
  ant_value_t subscription = js_get_slot(O, SLOT_DATA);
  if (!is_special_object(subscription)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  ant_value_t observer = js_get_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER);
  if (!is_special_object(observer)) return js_mkundef();
  
  ant_value_t nextMethod = js_get(js, observer, "next");
  if (is_callable(nextMethod)) {
    ant_value_t value = (nargs > 0) ? args[0] : js_mkundef();
    ant_value_t call_args[1] = {value};
    ant_value_t result = sv_vm_call(js->vm, js, nextMethod, observer, call_args, 1, NULL, false);
    if (vtype(result) == T_ERR) fprintf(stderr, "Error in observer.next: %s\n", js_str(js, result));
  }
  
  return js_mkundef();
}

static ant_value_t js_subobs_error(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t O = js_getthis(js);
  
  if (!is_special_object(O)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.error called on non-object");
  }
  
  ant_value_t subscription = js_get_slot(O, SLOT_DATA);
  if (!is_special_object(subscription)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  ant_value_t observer = js_get_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER);
  js_set_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER, js_mkundef());
  
  if (is_special_object(observer)) {
    ant_value_t errorMethod = js_get(js, observer, "error");
    if (is_callable(errorMethod)) {
      ant_value_t exception = (nargs > 0) ? args[0] : js_mkundef();
      ant_value_t call_args[1] = {exception};
      ant_value_t result = sv_vm_call(js->vm, js, errorMethod, observer, call_args, 1, NULL, false);
      if (vtype(result) == T_ERR) fprintf(stderr, "Error in observer.error: %s\n", js_str(js, result));
    }
  }
  
  cleanup_subscription(js, subscription);
  return js_mkundef();
}

static ant_value_t js_subobs_complete(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t O = js_getthis(js);
  
  if (!is_special_object(O)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.complete called on non-object");
  }
  
  ant_value_t subscription = js_get_slot(O, SLOT_DATA);
  if (!is_special_object(subscription)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  ant_value_t observer = js_get_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER);
  js_set_slot(subscription, SLOT_SUBSCRIPTION_OBSERVER, js_mkundef());
  
  if (is_special_object(observer)) {
    ant_value_t completeMethod = js_get(js, observer, "complete");
    if (is_callable(completeMethod)) {
      ant_value_t result = sv_vm_call(js->vm, js, completeMethod, observer, NULL, 0, NULL, false);
      if (vtype(result) == T_ERR) fprintf(stderr, "Error in observer.complete: %s\n", js_str(js, result));
    }
  }
  
  cleanup_subscription(js, subscription);
  return js_mkundef();
}

static ant_value_t create_subscription_observer(ant_t *js, ant_value_t subscription) {
  ant_value_t subobs = js_mkobj(js);
  
  js_set_slot(subobs, SLOT_DATA, subscription);
  js_set(js, subobs, "next", js_mkfun(js_subobs_next));
  js_set(js, subobs, "error", js_mkfun(js_subobs_error));
  js_set(js, subobs, "complete", js_mkfun(js_subobs_complete));
  js_set_sym(js, subobs, get_toStringTag_sym(), js_mkstr(js, "SubscriptionObserver", 20));
  
  ant_value_t closed_getter = js_mkfun(js_subobs_get_closed);
  js_set_getter_desc(js, subobs, "closed", 6, closed_getter, JS_DESC_E | JS_DESC_C);
  
  return subobs;
}

static ant_value_t js_cleanup_fn(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  ant_value_t F = js_getcurrentfunc(js);
  ant_value_t subscription = js_get_slot(F, SLOT_DATA);
  
  if (!is_special_object(subscription)) return js_mkundef();
  
  ant_value_t unsubscribe = js_get(js, subscription, "unsubscribe");
  if (is_callable(unsubscribe)) {
    return sv_vm_call(js->vm, js, unsubscribe, subscription, NULL, 0, NULL, false);
  }
  
  return js_mkundef();
}

static ant_value_t execute_subscriber(ant_t *js, ant_value_t subscriber, ant_value_t observer) {
  ant_value_t call_args[1] = {observer};
  ant_value_t subscriberResult = sv_vm_call(js->vm, js, subscriber, js_mkundef(), call_args, 1, NULL, false);
  
  if (vtype(subscriberResult) == T_ERR) return subscriberResult;
  if (vtype(subscriberResult) == T_NULL || vtype(subscriberResult) == T_UNDEF) return js_mkundef();
  if (is_callable(subscriberResult)) return subscriberResult;
  
  if (is_special_object(subscriberResult)) {
    ant_value_t result = js_get(js, subscriberResult, "unsubscribe");
    if (vtype(result) == T_UNDEF) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Subscriber return value must have an unsubscribe method");
    }
    
    ant_value_t cleanupFunction = js_mkobj(js);
    js_set_slot(cleanupFunction, SLOT_DATA, subscriberResult);
    js_set_slot(cleanupFunction, SLOT_CFUNC, js_mkfun(js_cleanup_fn));
    return js_obj_to_func(cleanupFunction);
  }
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "Subscriber must return a function, an object with unsubscribe, or undefined");
}

static ant_value_t js_observable_subscribe(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t O = js_getthis(js);
  
  if (!is_special_object(O)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable.prototype.subscribe called on non-object");
  }
  
  ant_value_t subscriber = js_get_slot(O, SLOT_OBSERVABLE_SUBSCRIBER);
  if (!is_callable(subscriber)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable has no [[Subscriber]] internal slot");
  }
  
  ant_value_t observer;
  
  if (nargs > 0 && is_callable(args[0])) {
    ant_value_t nextCallback = args[0];
    ant_value_t errorCallback = (nargs > 1) ? args[1] : js_mkundef();
    ant_value_t completeCallback = (nargs > 2) ? args[2] : js_mkundef();
    
    observer = js_mkobj(js);
    js_set(js, observer, "next", nextCallback);
    js_set(js, observer, "error", errorCallback);
    js_set(js, observer, "complete", completeCallback);
  } else if (nargs > 0 && is_special_object(args[0])) {
    observer = args[0];
  } else observer = js_mkobj(js);
  
  ant_value_t subscription = create_subscription(js, observer);
  setup_subscription_methods(js, subscription);
  
  ant_value_t start = js_get(js, observer, "start");
  if (is_callable(start)) {
    ant_value_t start_args[1] = {subscription};
    ant_value_t result = sv_vm_call(js->vm, js, start, observer, start_args, 1, NULL, false);
    if (vtype(result) == T_ERR) {
      fprintf(stderr, "Error in observer.start: %s\n", js_str(js, result));
    }
    if (subscription_closed(js, subscription)) return subscription;
  }
  
  ant_value_t subscriptionObserver = create_subscription_observer(js, subscription);
  ant_value_t subscriberResult = execute_subscriber(js, subscriber, subscriptionObserver);
  
  if (vtype(subscriberResult) == T_ERR) {
    ant_value_t thrown_error = js->thrown_value;
    js->thrown_value = js_mkundef();
    js->thrown_exists = false;
    
    ant_value_t error_args[1] = {thrown_error};
    ant_value_t error_method = js_get(js, subscriptionObserver, "error");
    if (is_callable(error_method)) sv_vm_call(js->vm, js, error_method, subscriptionObserver, error_args, 1, NULL, false);
  } else js_set_slot_wb(js, subscription, SLOT_SUBSCRIPTION_CLEANUP, subscriberResult);
  
  if (subscription_closed(js, subscription)) cleanup_subscription(js, subscription);
  
  return subscription;
}

static ant_value_t js_observable_symbol_observable(ant_t *js, ant_value_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_getthis(js);
}

static ant_value_t js_observable_constructor(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable constructor requires a subscriber function");
  }
  
  ant_value_t subscriber = args[0];
  if (!is_callable(subscriber)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable subscriber must be a function");
  }
  
  ant_value_t proto = js_get_ctor_proto(js, "Observable", 10);
  ant_value_t observable = js_mkobj(js);
  
  js_set_proto_init(observable, proto);
  js_set_slot(observable, SLOT_OBSERVABLE_SUBSCRIBER, subscriber);
  
  return observable;
}

static ant_value_t js_of_subscriber(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t F = js_getcurrentfunc(js);
  ant_value_t items = js_get_slot(F, SLOT_DATA);
  
  if (nargs < 1) return js_mkundef();
  
  ant_value_t observer = args[0];
  ant_value_t subscription = js_get_slot(observer, SLOT_DATA);
  
  ant_value_t length_val = js_get(js, items, "length");
  int length = (vtype(length_val) == T_NUM) ? (int)js_getnum(length_val) : 0;
  
  for (int i = 0; i < length; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%d", i);
    ant_value_t value = js_get(js, items, key);
    
    ant_value_t next = js_get(js, observer, "next");
    if (is_callable(next)) {
      ant_value_t next_args[1] = {value};
      sv_vm_call(js->vm, js, next, observer, next_args, 1, NULL, false);
    }
    
    if (is_special_object(subscription) && subscription_closed(js, subscription)) return js_mkundef();
  }
  
  ant_value_t complete = js_get(js, observer, "complete");
  if (is_callable(complete)) sv_vm_call(js->vm, js, complete, observer, NULL, 0, NULL, false);
  
  return js_mkundef();
}

static ant_value_t js_observable_of(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t items = js_mkarr(js);
  for (int i = 0; i < nargs; i++) js_arr_push(js, items, args[i]);
  
  ant_value_t subscriber_func = js_heavy_mkfun(js, js_of_subscriber, items);
  ant_value_t ctor_args[1] = {subscriber_func};
  
  return js_observable_constructor(js, ctor_args, 1);
}

static ant_value_t js_from_delegating(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t F = js_getcurrentfunc(js);
  
  ant_value_t observable = js_get_slot(F, SLOT_DATA);
  if (!is_special_object(observable)) return js_mkundef();
  
  ant_value_t subscribe = js_get(js, observable, "subscribe");
  if (is_callable(subscribe)) {
    return sv_vm_call(js->vm, js, subscribe, observable, args, nargs, NULL, false);
  }
  
  return js_mkundef();
}

static ant_value_t js_from_iteration(ant_t *js, ant_value_t *args, int nargs) {
  ant_value_t F = js_getcurrentfunc(js);
  ant_value_t data = js_get_slot(F, SLOT_DATA);
  
  ant_value_t iterable = js_get(js, data, "iterable");
  ant_value_t iteratorMethod = js_get(js, data, "iteratorMethod");
  
  if (nargs < 1) return js_mkundef();
  
  ant_value_t observer = args[0];
  ant_value_t subscription = js_get_slot(observer, SLOT_DATA);
  
  if (!is_callable(iteratorMethod)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object is not iterable");
  }
  
  ant_value_t iterator = sv_vm_call(js->vm, js, iteratorMethod, iterable, NULL, 0, NULL, false);
  if (!is_special_object(iterator)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator must return an object");
  }
  
  ant_value_t nextMethod = js_getprop_fallback(js, iterator, "next");
  if (!is_callable(nextMethod)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator must have a next method");
  }
  
  while (true) {
    ant_value_t next = sv_vm_call(js->vm, js, nextMethod, iterator, NULL, 0, NULL, false);
    if (vtype(next) == T_ERR) return next;
    
    ant_value_t done = js_get(js, next, "done");
    if (js_truthy(js, done)) {
      ant_value_t complete = js_get(js, observer, "complete");
      if (is_callable(complete)) sv_vm_call(js->vm, js, complete, observer, NULL, 0, NULL, false);
      return js_mkundef();
    }
    
    ant_value_t nextValue = js_get(js, next, "value");
    ant_value_t obs_next = js_get(js, observer, "next");
    if (is_callable(obs_next)) {
      ant_value_t next_args[1] = {nextValue};
      sv_vm_call(js->vm, js, obs_next, observer, next_args, 1, NULL, false);
    }
    
    if (is_special_object(subscription) && subscription_closed(js, subscription)) {
      ant_value_t returnMethod = js_getprop_fallback(js, iterator, "return");
      if (is_callable(returnMethod)) sv_vm_call(js->vm, js, returnMethod, iterator, NULL, 0, NULL, false);
      return js_mkundef();
    }
  }
}

static ant_value_t js_observable_from(ant_t *js, ant_value_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Observable.from requires an argument");
  ant_value_t x = args[0];
  
  if (vtype(x) == T_NULL || vtype(x) == T_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert null or undefined to observable");
  }
  
  ant_value_t observableMethod = js_get_sym(js, x, get_observable_sym());
  
  if (is_callable(observableMethod)) {
    ant_value_t observable = sv_vm_call(js->vm, js, observableMethod, x, NULL, 0, NULL, false);
    
    if (!is_special_object(observable)) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "@@observable must return an object");
    }
    
    ant_value_t existing_subscriber = js_get_slot(observable, SLOT_OBSERVABLE_SUBSCRIBER);
    if (is_callable(existing_subscriber)) return observable;
    
    ant_value_t subscriber_func = js_heavy_mkfun(js, js_from_delegating, observable);
    ant_value_t ctor_args[1] = {subscriber_func};
    
    return js_observable_constructor(js, ctor_args, 1);
  }
  
  ant_value_t iteratorMethod = js_get_sym(js, x, get_iterator_sym());
  
  if (!is_callable(iteratorMethod) && vtype(x) == T_ARR) {
    ant_value_t array_ctor = js_get(js, js_glob(js), "Array");
    ant_value_t array_proto = js_get(js, array_ctor, "prototype");
    iteratorMethod = js_get_sym(js, array_proto, get_iterator_sym());
  }
  
  if (!is_callable(iteratorMethod)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object is not observable or iterable");
  }
  
  ant_value_t data = js_mkobj(js);
  js_set(js, data, "iterable", x);
  js_set(js, data, "iteratorMethod", iteratorMethod);
  
  ant_value_t subscriber_func = js_heavy_mkfun(js, js_from_iteration, data);
  ant_value_t ctor_args[1] = {subscriber_func};
  
  return js_observable_constructor(js, ctor_args, 1);
}

void init_observable_module(void) {
  ant_t *js = rt->js;
  ant_value_t global = js_glob(js);
  
  ant_value_t observable_ctor = js_mkobj(js);
  ant_value_t observable_proto = js_mkobj(js);
  
  js_set(js, observable_proto, "subscribe", js_mkfun(js_observable_subscribe));
  js_set_sym(js, observable_proto, get_observable_sym(), js_mkfun(js_observable_symbol_observable));
  js_set_sym(js, observable_proto, get_toStringTag_sym(), js_mkstr(js, "Observable", 10));
  
  js_set_slot(observable_ctor, SLOT_CFUNC, js_mkfun(js_observable_constructor));
  js_mkprop_fast(js, observable_ctor, "prototype", 9, observable_proto);
  js_mkprop_fast(js, observable_ctor, "name", 4, ANT_STRING("Observable"));
  js_set_descriptor(js, observable_ctor, "name", 4, 0);
  js_set(js, observable_ctor, "of", js_mkfun(js_observable_of));
  js_set(js, observable_ctor, "from", js_mkfun(js_observable_from));
  
  ant_value_t Observable = js_obj_to_func(observable_ctor);
  js_set(js, observable_proto, "constructor", Observable);
  js_set(js, global, "Observable", Observable);
}
