#pragma once

#include <stddef.h>

#define Ant_List_IsLinked(node) ((node)->pprev != NULL)

#define Ant_List_Insert(head, node) ({                    \
  typeof(head) _head = (head);                            \
  typeof(node) _node = (node);                            \
  if (!_node->pprev) {                                    \
    _node->next = *_head;                                 \
    if (_node->next) _node->next->pprev = &_node->next;   \
    _node->pprev = _head;                                 \
    *_head = _node;                                       \
  }                                                       \
})

#define Ant_List_Remove(node) ({                          \
  typeof(node) _node = (node);                            \
  if (_node->pprev) {                                     \
    *_node->pprev = _node->next;                          \
    if (_node->next) _node->next->pprev = _node->pprev;   \
    _node->next = NULL;                                   \
    _node->pprev = NULL;                                  \
  }                                                       \
})
