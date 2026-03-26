#ifndef URL_H
#define URL_H

#include <stddef.h>
#include "types.h"

typedef struct {
  char *protocol;
  char *username;
  char *password;
  char *hostname;
  char *port;
  char *pathname;
  char *search;
  char *hash;
} url_state_t;


void init_url_module(void);
void url_state_clear(url_state_t *s);
void url_free_state(url_state_t *s);
bool usp_is_urlsearchparams(ant_t *js, ant_value_t obj);

ant_value_t url_library(ant_t *js);
url_state_t *url_get_state(ant_value_t obj);
ant_value_t make_url_obj(ant_t *js, url_state_t *s);

char *build_href(const url_state_t *s);
char *usp_serialize(ant_t *js, ant_value_t usp);
char *form_urlencode(const char *str);
char *form_urlencode_n(const char *str, size_t len);
char *form_urldecode(const char *str);
char *url_decode_component(const char *str);

int parse_url_to_state(
  const char *url_str, 
  const char *base_str, url_state_t *s
);

#endif
