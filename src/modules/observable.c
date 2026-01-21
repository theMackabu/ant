#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "internal.h"
#include "runtime.h"

#include "modules/symbol.h"
#include "modules/observable.h"

static inline bool is_callable(jsval_t val) {
  uint8_t t = vtype(val);
  return t == T_FUNC || t == T_CFUNC;
}

static bool subscription_closed(struct js *js, jsval_t subscription) {
  jsval_t observer = js_get_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER);
  return js_type(observer) == JS_UNDEF;
}

static void cleanup_subscription(struct js *js, jsval_t subscription) {
  jsval_t cleanup = js_get_slot(js, subscription, SLOT_SUBSCRIPTION_CLEANUP);
  if (js_type(cleanup) == JS_UNDEF) return;
  if (!is_callable(cleanup)) return;
  
  js_set_slot(js, subscription, SLOT_SUBSCRIPTION_CLEANUP, js_mkundef());
  jsval_t result = js_call(js, cleanup, NULL, 0);
  
  if (js_type(result) == JS_ERR) fprintf(stderr, "Error in subscription cleanup: %s\n", js_str(js, result));
}

static jsval_t create_subscription(struct js *js, jsval_t observer) {
  jsval_t subscription = js_mkobj(js);
  js_set_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER, observer);
  js_set_slot(js, subscription, SLOT_SUBSCRIPTION_CLEANUP, js_mkundef());
  js_set(js, subscription, get_toStringTag_sym_key(), js_mkstr(js, "Subscription", 12));
  return subscription;
}

static jsval_t js_subscription_get_closed(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t subscription = js_getthis(js);
  
  if (js_type(subscription) != JS_OBJ) return js_mkerr_typed(js, JS_ERR_TYPE, "Subscription.closed getter called on non-object");
  return subscription_closed(js, subscription) ? js_mktrue() : js_mkfalse();
}

static jsval_t js_subscription_unsubscribe(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t subscription = js_getthis(js);
  
  if (js_type(subscription) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Subscription.unsubscribe called on non-object");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  js_set_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER, js_mkundef());
  cleanup_subscription(js, subscription);
  
  return js_mkundef();
}

static void setup_subscription_methods(struct js *js, jsval_t subscription) {
  js_set(js, subscription, "unsubscribe", js_mkfun(js_subscription_unsubscribe));
  jsval_t closed_getter = js_mkfun(js_subscription_get_closed);
  js_set_getter_desc(js, subscription, "closed", 6, closed_getter, JS_DESC_E | JS_DESC_C);
}

static jsval_t js_subobs_get_closed(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t O = js_getthis(js);
  
  if (js_type(O) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.closed getter called on non-object");
  }
  
  jsval_t subscription = js_get_slot(js, O, SLOT_DATA);
  if (js_type(subscription) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  return subscription_closed(js, subscription) ? js_mktrue() : js_mkfalse();
}

static jsval_t js_subobs_next(struct js *js, jsval_t *args, int nargs) {
  jsval_t O = js_getthis(js);
  
  if (js_type(O) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.next called on non-object");
  }
  
  jsval_t subscription = js_get_slot(js, O, SLOT_DATA);
  if (js_type(subscription) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  jsval_t observer = js_get_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER);
  if (js_type(observer) != JS_OBJ) return js_mkundef();
  
  jsval_t nextMethod = js_get(js, observer, "next");
  if (is_callable(nextMethod)) {
    jsval_t value = (nargs > 0) ? args[0] : js_mkundef();
    jsval_t call_args[1] = {value};
    jsval_t result = js_call_with_this(js, nextMethod, observer, call_args, 1);
    if (js_type(result) == JS_ERR) fprintf(stderr, "Error in observer.next: %s\n", js_str(js, result));
  }
  
  return js_mkundef();
}

static jsval_t js_subobs_error(struct js *js, jsval_t *args, int nargs) {
  jsval_t O = js_getthis(js);
  
  if (js_type(O) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.error called on non-object");
  }
  
  jsval_t subscription = js_get_slot(js, O, SLOT_DATA);
  if (js_type(subscription) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  jsval_t observer = js_get_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER);
  js_set_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER, js_mkundef());
  
  if (js_type(observer) == JS_OBJ) {
    jsval_t errorMethod = js_get(js, observer, "error");
    if (is_callable(errorMethod)) {
      jsval_t exception = (nargs > 0) ? args[0] : js_mkundef();
      jsval_t call_args[1] = {exception};
      jsval_t result = js_call_with_this(js, errorMethod, observer, call_args, 1);
      if (js_type(result) == JS_ERR) fprintf(stderr, "Error in observer.error: %s\n", js_str(js, result));
    }
  }
  
  cleanup_subscription(js, subscription);
  return js_mkundef();
}

static jsval_t js_subobs_complete(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t O = js_getthis(js);
  
  if (js_type(O) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "SubscriptionObserver.complete called on non-object");
  }
  
  jsval_t subscription = js_get_slot(js, O, SLOT_DATA);
  if (js_type(subscription) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Invalid SubscriptionObserver");
  }
  
  if (subscription_closed(js, subscription)) return js_mkundef();
  
  jsval_t observer = js_get_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER);
  js_set_slot(js, subscription, SLOT_SUBSCRIPTION_OBSERVER, js_mkundef());
  
  if (js_type(observer) == JS_OBJ) {
    jsval_t completeMethod = js_get(js, observer, "complete");
    if (is_callable(completeMethod)) {
      jsval_t result = js_call_with_this(js, completeMethod, observer, NULL, 0);
      if (js_type(result) == JS_ERR) fprintf(stderr, "Error in observer.complete: %s\n", js_str(js, result));
    }
  }
  
  cleanup_subscription(js, subscription);
  return js_mkundef();
}

static jsval_t create_subscription_observer(struct js *js, jsval_t subscription) {
  jsval_t subobs = js_mkobj(js);
  
  js_set_slot(js, subobs, SLOT_DATA, subscription);
  js_set(js, subobs, "next", js_mkfun(js_subobs_next));
  js_set(js, subobs, "error", js_mkfun(js_subobs_error));
  js_set(js, subobs, "complete", js_mkfun(js_subobs_complete));
  js_set(js, subobs, get_toStringTag_sym_key(), js_mkstr(js, "SubscriptionObserver", 20));
  
  jsval_t closed_getter = js_mkfun(js_subobs_get_closed);
  js_set_getter_desc(js, subobs, "closed", 6, closed_getter, JS_DESC_E | JS_DESC_C);
  
  return subobs;
}

static jsval_t js_cleanup_fn(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  jsval_t F = js_getcurrentfunc(js);
  jsval_t subscription = js_get_slot(js, F, SLOT_DATA);
  
  if (js_type(subscription) != JS_OBJ) return js_mkundef();
  
  jsval_t unsubscribe = js_get(js, subscription, "unsubscribe");
  if (is_callable(unsubscribe)) {
    return js_call_with_this(js, unsubscribe, subscription, NULL, 0);
  }
  
  return js_mkundef();
}

static jsval_t execute_subscriber(struct js *js, jsval_t subscriber, jsval_t observer) {
  jsval_t call_args[1] = {observer};
  jsval_t subscriberResult = js_call(js, subscriber, call_args, 1);
  
  if (js_type(subscriberResult) == JS_ERR) return subscriberResult;
  if (js_type(subscriberResult) == JS_NULL || js_type(subscriberResult) == JS_UNDEF) return js_mkundef();
  if (is_callable(subscriberResult)) return subscriberResult;
  
  if (js_type(subscriberResult) == JS_OBJ) {
    jsval_t result = js_get(js, subscriberResult, "unsubscribe");
    if (js_type(result) == JS_UNDEF) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "Subscriber return value must have an unsubscribe method");
    }
    
    jsval_t cleanupFunction = js_mkobj(js);
    js_set_slot(js, cleanupFunction, SLOT_DATA, subscriberResult);
    js_set_slot(js, cleanupFunction, SLOT_CFUNC, js_mkfun(js_cleanup_fn));
    return js_obj_to_func(cleanupFunction);
  }
  
  return js_mkerr_typed(js, JS_ERR_TYPE, "Subscriber must return a function, an object with unsubscribe, or undefined");
}

static jsval_t js_observable_subscribe(struct js *js, jsval_t *args, int nargs) {
  jsval_t O = js_getthis(js);
  
  if (js_type(O) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable.prototype.subscribe called on non-object");
  }
  
  jsval_t subscriber = js_get_slot(js, O, SLOT_OBSERVABLE_SUBSCRIBER);
  if (!is_callable(subscriber)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable has no [[Subscriber]] internal slot");
  }
  
  jsval_t observer;
  
  if (nargs > 0 && is_callable(args[0])) {
    jsval_t nextCallback = args[0];
    jsval_t errorCallback = (nargs > 1) ? args[1] : js_mkundef();
    jsval_t completeCallback = (nargs > 2) ? args[2] : js_mkundef();
    
    observer = js_mkobj(js);
    js_set(js, observer, "next", nextCallback);
    js_set(js, observer, "error", errorCallback);
    js_set(js, observer, "complete", completeCallback);
  } else if (nargs > 0 && js_type(args[0]) == JS_OBJ) {
    observer = args[0];
  } else observer = js_mkobj(js);
  
  jsval_t subscription = create_subscription(js, observer);
  setup_subscription_methods(js, subscription);
  
  jsval_t start = js_get(js, observer, "start");
  if (is_callable(start)) {
    jsval_t start_args[1] = {subscription};
    jsval_t result = js_call_with_this(js, start, observer, start_args, 1);
    if (js_type(result) == JS_ERR) {
      fprintf(stderr, "Error in observer.start: %s\n", js_str(js, result));
    }
    if (subscription_closed(js, subscription)) return subscription;
  }
  
  jsval_t subscriptionObserver = create_subscription_observer(js, subscription);
  jsval_t subscriberResult = execute_subscriber(js, subscriber, subscriptionObserver);
  
  if (js_type(subscriberResult) == JS_ERR) {
    jsval_t thrown_error = js->thrown_value;
    js->thrown_value = js_mkundef();
    js->flags &= ~F_THROW;
    
    jsval_t error_args[1] = {thrown_error};
    jsval_t error_method = js_get(js, subscriptionObserver, "error");
    if (is_callable(error_method)) js_call_with_this(js, error_method, subscriptionObserver, error_args, 1);
  } else js_set_slot(js, subscription, SLOT_SUBSCRIPTION_CLEANUP, subscriberResult);
  
  if (subscription_closed(js, subscription)) cleanup_subscription(js, subscription);
  
  return subscription;
}

static jsval_t js_observable_symbol_observable(struct js *js, jsval_t *args, int nargs) {
  (void)args; (void)nargs;
  return js_getthis(js);
}

static jsval_t js_observable_constructor(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable constructor requires a subscriber function");
  }
  
  jsval_t subscriber = args[0];
  if (!is_callable(subscriber)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Observable subscriber must be a function");
  }
  
  jsval_t proto = js_get_ctor_proto(js, "Observable", 10);
  jsval_t observable = js_mkobj(js);
  
  js_set_proto(js, observable, proto);
  js_set_slot(js, observable, SLOT_OBSERVABLE_SUBSCRIBER, subscriber);
  
  return observable;
}

static jsval_t js_of_subscriber(struct js *js, jsval_t *args, int nargs) {
  jsval_t F = js_getcurrentfunc(js);
  jsval_t items = js_get_slot(js, F, SLOT_DATA);
  
  if (nargs < 1) return js_mkundef();
  
  jsval_t observer = args[0];
  jsval_t subscription = js_get_slot(js, observer, SLOT_DATA);
  
  jsval_t length_val = js_get(js, items, "length");
  int length = (js_type(length_val) == JS_NUM) ? (int)js_getnum(length_val) : 0;
  
  for (int i = 0; i < length; i++) {
    char key[16];
    snprintf(key, sizeof(key), "%d", i);
    jsval_t value = js_get(js, items, key);
    
    jsval_t next = js_get(js, observer, "next");
    if (is_callable(next)) {
      jsval_t next_args[1] = {value};
      js_call_with_this(js, next, observer, next_args, 1);
    }
    
    if (js_type(subscription) == JS_OBJ && subscription_closed(js, subscription)) return js_mkundef();
  }
  
  jsval_t complete = js_get(js, observer, "complete");
  if (is_callable(complete)) js_call_with_this(js, complete, observer, NULL, 0);
  
  return js_mkundef();
}

static jsval_t js_observable_of(struct js *js, jsval_t *args, int nargs) {
  jsval_t items = js_mkarr(js);
  for (int i = 0; i < nargs; i++) js_arr_push(js, items, args[i]);
  
  jsval_t subscriber_func = js_heavy_mkfun(js, js_of_subscriber, items);
  jsval_t ctor_args[1] = {subscriber_func};
  
  return js_observable_constructor(js, ctor_args, 1);
}

static jsval_t js_from_delegating(struct js *js, jsval_t *args, int nargs) {
  jsval_t F = js_getcurrentfunc(js);
  
  jsval_t observable = js_get_slot(js, F, SLOT_DATA);
  if (js_type(observable) != JS_OBJ) return js_mkundef();
  
  jsval_t subscribe = js_get(js, observable, "subscribe");
  if (is_callable(subscribe)) {
    return js_call_with_this(js, subscribe, observable, args, nargs);
  }
  
  return js_mkundef();
}

static jsval_t js_from_iteration(struct js *js, jsval_t *args, int nargs) {
  jsval_t F = js_getcurrentfunc(js);
  jsval_t data = js_get_slot(js, F, SLOT_DATA);
  
  jsval_t iterable = js_get(js, data, "iterable");
  jsval_t iteratorMethod = js_get(js, data, "iteratorMethod");
  
  if (nargs < 1) return js_mkundef();
  
  jsval_t observer = args[0];
  jsval_t subscription = js_get_slot(js, observer, SLOT_DATA);
  
  if (!is_callable(iteratorMethod)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object is not iterable");
  }
  
  jsval_t iterator = js_call_with_this(js, iteratorMethod, iterable, NULL, 0);
  if (js_type(iterator) != JS_OBJ) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator must return an object");
  }
  
  jsval_t nextMethod = js_get(js, iterator, "next");
  if (!is_callable(nextMethod)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Iterator must have a next method");
  }
  
  while (true) {
    jsval_t next = js_call_with_this(js, nextMethod, iterator, NULL, 0);
    if (js_type(next) == JS_ERR) return next;
    
    jsval_t done = js_get(js, next, "done");
    if (js_truthy(js, done)) {
      jsval_t complete = js_get(js, observer, "complete");
      if (is_callable(complete)) js_call_with_this(js, complete, observer, NULL, 0);
      return js_mkundef();
    }
    
    jsval_t nextValue = js_get(js, next, "value");
    jsval_t obs_next = js_get(js, observer, "next");
    if (is_callable(obs_next)) {
      jsval_t next_args[1] = {nextValue};
      js_call_with_this(js, obs_next, observer, next_args, 1);
    }
    
    if (js_type(subscription) == JS_OBJ && subscription_closed(js, subscription)) {
      jsval_t returnMethod = js_get(js, iterator, "return");
      if (is_callable(returnMethod)) js_call_with_this(js, returnMethod, iterator, NULL, 0);
      return js_mkundef();
    }
  }
}

static jsval_t js_observable_from(struct js *js, jsval_t *args, int nargs) {
  if (nargs < 1) return js_mkerr_typed(js, JS_ERR_TYPE, "Observable.from requires an argument");
  jsval_t x = args[0];
  
  if (js_type(x) == JS_NULL || js_type(x) == JS_UNDEF) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Cannot convert null or undefined to observable");
  }
  
  jsval_t observableMethod = js_getprop_proto(js, x, get_observable_sym_key());
  
  if (is_callable(observableMethod)) {
    jsval_t observable = js_call_with_this(js, observableMethod, x, NULL, 0);
    
    if (js_type(observable) != JS_OBJ) {
      return js_mkerr_typed(js, JS_ERR_TYPE, "@@observable must return an object");
    }
    
    jsval_t existing_subscriber = js_get_slot(js, observable, SLOT_OBSERVABLE_SUBSCRIBER);
    if (is_callable(existing_subscriber)) return observable;
    
    jsval_t subscriber_func = js_heavy_mkfun(js, js_from_delegating, observable);
    jsval_t ctor_args[1] = {subscriber_func};
    
    return js_observable_constructor(js, ctor_args, 1);
  }
  
  jsval_t iteratorMethod = js_get(js, x, get_iterator_sym_key());
  
  if (!is_callable(iteratorMethod) && vtype(x) == T_ARR) {
    jsval_t array_ctor = js_get(js, js_glob(js), "Array");
    jsval_t array_proto = js_get(js, array_ctor, "prototype");
    iteratorMethod = js_get(js, array_proto, get_iterator_sym_key());
  }
  
  if (!is_callable(iteratorMethod)) {
    return js_mkerr_typed(js, JS_ERR_TYPE, "Object is not observable or iterable");
  }
  
  jsval_t data = js_mkobj(js);
  js_set(js, data, "iterable", x);
  js_set(js, data, "iteratorMethod", iteratorMethod);
  
  jsval_t subscriber_func = js_heavy_mkfun(js, js_from_iteration, data);
  jsval_t ctor_args[1] = {subscriber_func};
  
  return js_observable_constructor(js, ctor_args, 1);
}

void init_observable_module(void) {
  struct js *js = rt->js;
  jsval_t global = js_glob(js);
  
  jsval_t observable_ctor = js_mkobj(js);
  jsval_t observable_proto = js_mkobj(js);
  
  js_set(js, observable_proto, "subscribe", js_mkfun(js_observable_subscribe));
  js_set(js, observable_proto, get_observable_sym_key(), js_mkfun(js_observable_symbol_observable));
  js_set(js, observable_proto, get_toStringTag_sym_key(), js_mkstr(js, "Observable", 10));
  
  js_set_slot(js, observable_ctor, SLOT_CFUNC, js_mkfun(js_observable_constructor));
  js_mkprop_fast(js, observable_ctor, "prototype", 9, observable_proto);
  js_mkprop_fast(js, observable_ctor, "name", 4, ANT_STRING("Observable"));
  js_set_descriptor(js, observable_ctor, "name", 4, 0);
  js_set(js, observable_ctor, "of", js_mkfun(js_observable_of));
  js_set(js, observable_ctor, "from", js_mkfun(js_observable_from));
  
  jsval_t Observable = js_obj_to_func(observable_ctor);
  js_set(js, observable_proto, "constructor", Observable);
  js_set(js, global, "Observable", Observable);
}
