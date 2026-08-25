#include "internal.h"

BOOL OptionBool(ant_t *js, ant_value_t options, const char *name, BOOL fallback) {
  if (!is_object_type(options)) return fallback;
  ant_value_t value = js_get(js, options, name);
  return vtype(value) == kTypeBool ? js_truthy(js, value) : fallback;
}

double OptionNumber(ant_t *js, ant_value_t options, const char *name, double fallback) {
  if (!is_object_type(options)) return fallback;
  ant_value_t value = js_get(js, options, name);
  return vtype(value) == kTypeNumber ? js_getnum(value) : fallback;
}

NSString *OptionString(ant_t *js, ant_value_t options, const char *name) {
  if (!is_object_type(options)) return nil;
  ant_value_t value = js_get(js, options, name);
  if (vtype(value) != kTypeString) return nil;
  size_t length = 0;
  const char *text = js_getstr(js, value, &length);
  return [[NSString alloc] initWithBytes:text length:length encoding:NSUTF8StringEncoding];
}

static BOOL IsValidIpcChannel(NSString *channel) {
  if (channel.length == 0 || channel.length > 128) return NO;
  NSCharacterSet *allowed = [NSCharacterSet
    characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:._-"];
  return [channel rangeOfCharacterFromSet:allowed.invertedSet].location == NSNotFound;
}

NSString *CapabilityManifest(ant_t *js, ant_value_t options, NSString **error) {
  if (!is_object_type(options)) return @"";
  ant_value_t web_preferences = js_get(js, options, "webPreferences");
  if (!is_object_type(web_preferences)) return @"";
  ant_value_t capabilities = js_get(js, web_preferences, "capabilities");
  if (vtype(capabilities) == kTypeUndefined) return @"";
  if (!is_array_value(capabilities)) {
    if (error) *error = @"webPreferences.capabilities must be an array";
    return nil;
  }

  NSMutableOrderedSet<NSString *> *grants = [NSMutableOrderedSet orderedSet];
  ant_offset_t count = js_arr_len(js, capabilities);
  for (ant_offset_t index = 0; index < count; index++) {
    ant_value_t capability = js_arr_get(js, capabilities, index);
    NSString *channel = OptionString(js, capability, "channel");
    ant_value_t access = is_object_type(capability) ? js_get(js, capability, "access") : js_mkundef();
    if (!IsValidIpcChannel(channel) || !is_array_value(access)) {
      if (error) { *error = @"each IPC capability needs a safe channel and access array"; }
      return nil;
    }
    ant_offset_t access_count = js_arr_len(js, access);
    for (ant_offset_t access_index = 0; access_index < access_count; access_index++) {
      ant_value_t access_value = js_arr_get(js, access, access_index);
      if (vtype(access_value) != kTypeString) {
        if (error) *error = @"IPC access values must be strings";
        return nil;
      }
      size_t length = 0;
      const char *text = js_getstr(js, access_value, &length);
      NSString *kind = [[NSString alloc] initWithBytes:text length:length encoding:NSUTF8StringEncoding];
      if (![@[ @"send", @"invoke", @"receive" ] containsObject:kind]) {
        if (error) *error = @"IPC access must be send, invoke, or receive";
        return nil;
      }
      [grants addObject:[NSString stringWithFormat:@"%@:%@", kind, channel]];
    }
  }
  return [grants.array componentsJoinedByString:@";"];
}

NSColor *ColorFromHex(NSString *value) {
  if (![value hasPrefix:@"#"]) return nil;
  NSString *hex = [value substringFromIndex:1];
  if (hex.length != 6 && hex.length != 8) return nil;
  unsigned long long raw = 0;
  if (![[NSScanner scannerWithString:hex] scanHexLongLong:&raw]) return nil;
  CGFloat red, green, blue, alpha = 1;
  if (hex.length == 8) {
    red = ((raw >> 24) & 0xff) / 255.0;
    green = ((raw >> 16) & 0xff) / 255.0;
    blue = ((raw >> 8) & 0xff) / 255.0;
    alpha = (raw & 0xff) / 255.0;
  } else {
    red = ((raw >> 16) & 0xff) / 255.0;
    green = ((raw >> 8) & 0xff) / 255.0;
    blue = (raw & 0xff) / 255.0;
  }
  return [NSColor colorWithSRGBRed:red green:green blue:blue alpha:alpha];
}
