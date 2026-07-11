#ifndef ANT_DESKTOP_INTERNAL_H
#define ANT_DESKTOP_INTERNAL_H

#include "../../api/desktop_core.h"
#include "window.h"

extern NSMutableArray *g_menu_targets;
BOOL OptionBool(ant_t *js, ant_value_t options, const char *name, BOOL fallback);
double OptionNumber(ant_t *js, ant_value_t options, const char *name, double fallback);
NSString *OptionString(ant_t *js, ant_value_t options, const char *name);
NSString *CapabilityManifest(ant_t *js, ant_value_t options, NSString **error);
NSColor *ColorFromHex(NSString *value);

#endif
