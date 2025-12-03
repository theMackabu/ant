#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <limits.h>

typedef struct {
  char *placeholder;
  char *value;
} replacement_t;

typedef struct {
  char **paths;
  int count;
  int capacity;
} module_cache_t;

static char *resolve_path(const char *base_path, const char *import_path) {
  char *resolved = malloc(PATH_MAX);
  if (!resolved) return NULL;
  
  char *base_dir = strdup(base_path);
  char *dir = dirname(base_dir);
  
  snprintf(resolved, PATH_MAX, "%s/%s", dir, import_path);
  free(base_dir);
  
  return resolved;
}

static char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "r");
  if (!f) return NULL;
  
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  
  char *content = malloc(size + 1);
  if (!content) {
    fclose(f);
    return NULL;
  }
  
  fread(content, 1, size, f);
  content[size] = '\0';
  fclose(f);
  
  if (len) *len = size;
  return content;
}

static int module_already_processed(module_cache_t *cache, const char *path) {
  for (int i = 0; i < cache->count; i++) {
    if (strcmp(cache->paths[i], path) == 0) {
      return 1;
    }
  }
  return 0;
}

static void add_to_cache(module_cache_t *cache, const char *path) {
  if (cache->count >= cache->capacity) {
    cache->capacity = cache->capacity == 0 ? 10 : cache->capacity * 2;
    cache->paths = realloc(cache->paths, sizeof(char *) * cache->capacity);
  }
  
  cache->paths[cache->count] = strdup(path);
  cache->count++;
}

static char *process_snapshot_includes(const char *file_path, const char *content, size_t content_len, module_cache_t *cache, size_t *output_len);

static char *wrap_module(const char *content, size_t content_len, size_t *output_len) {
  size_t output_capacity = content_len + 1024;
  char *output = malloc(output_capacity);
  if (!output) return NULL;
  
  size_t output_pos = 0;
  const char *pos = content;
  const char *end = content + content_len;
  
  const char *iife_start = "(function() {\n";
  memcpy(output + output_pos, iife_start, strlen(iife_start));
  output_pos += strlen(iife_start);
  
  char **exports = NULL;
  int export_count = 0;
  int export_capacity = 0;
  
  while (pos < end) {
    const char *export_start = strstr(pos, "export ");
    if (!export_start || export_start >= end) {
      size_t remaining = end - pos;
      if (output_pos + remaining >= output_capacity) {
        output_capacity = (output_pos + remaining) * 2;
        output = realloc(output, output_capacity);
      }
      memcpy(output + output_pos, pos, remaining);
      output_pos += remaining;
      break;
    }
    
    memcpy(output + output_pos, pos, export_start - pos);
    output_pos += export_start - pos;
    
    const char *decl_start = export_start + 7;
    while (*decl_start == ' ') decl_start++;
    
    const char *const_pos = strstr(decl_start, "const ");
    const char *let_pos = strstr(decl_start, "let ");
    const char *var_pos = strstr(decl_start, "var ");
    const char *function_pos = strstr(decl_start, "function ");
    
    const char *keyword_start = NULL;
    const char *name_start = NULL;
    
    if (const_pos == decl_start) {
      keyword_start = const_pos;
      name_start = const_pos + 6;
    } else if (let_pos == decl_start) {
      keyword_start = let_pos;
      name_start = let_pos + 4;
    } else if (var_pos == decl_start) {
      keyword_start = var_pos;
      name_start = var_pos + 4;
    } else if (function_pos == decl_start) {
      keyword_start = function_pos;
      name_start = function_pos + 9;
    }
    
    if (keyword_start) {
      while (*name_start == ' ') name_start++;
      
      const char *name_end = name_start;
      while (*name_end && (*name_end == '_' || (*name_end >= 'a' && *name_end <= 'z') || 
             (*name_end >= 'A' && *name_end <= 'Z') || (*name_end >= '0' && *name_end <= '9'))) {
        name_end++;
      }
      
      size_t name_len = name_end - name_start;
      if (name_len > 0) {
        if (export_count >= export_capacity) {
          export_capacity = export_capacity == 0 ? 10 : export_capacity * 2;
          exports = realloc(exports, sizeof(char *) * export_capacity);
        }
        
        exports[export_count] = malloc(name_len + 1);
        memcpy(exports[export_count], name_start, name_len);
        exports[export_count][name_len] = '\0';
        export_count++;
      }
      
      const char *stmt_start = keyword_start;
      const char *line_end = strchr(export_start, ';');
      if (!line_end) line_end = strchr(export_start, '\n');
      if (line_end) {
        size_t stmt_len = line_end - stmt_start + 1;
        if (output_pos + stmt_len >= output_capacity) {
          output_capacity = (output_pos + stmt_len) * 2;
          output = realloc(output, output_capacity);
        }
        memcpy(output + output_pos, stmt_start, stmt_len);
        output_pos += stmt_len;
        pos = line_end + 1;
      } else {
        pos = export_start + 7;
      }
    } else {
      pos = export_start + 7;
    }
  }
  
  if (export_count > 0) {
    const char *return_start = "\nreturn { ";
    if (output_pos + strlen(return_start) >= output_capacity) {
      output_capacity = (output_pos + strlen(return_start) + 1024) * 2;
      output = realloc(output, output_capacity);
    }
    memcpy(output + output_pos, return_start, strlen(return_start));
    output_pos += strlen(return_start);
    
    for (int i = 0; i < export_count; i++) {
      size_t name_len = strlen(exports[i]);
      
      if (output_pos + name_len * 2 + 10 >= output_capacity) {
        output_capacity = (output_pos + name_len * 2 + 10) * 2;
        output = realloc(output, output_capacity);
      }
      
      memcpy(output + output_pos, exports[i], name_len);
      output_pos += name_len;
      
      if (i < export_count - 1) {
        memcpy(output + output_pos, ", ", 2);
        output_pos += 2;
      }
    }
    
    const char *return_end = " };\n})()";
    memcpy(output + output_pos, return_end, strlen(return_end));
    output_pos += strlen(return_end);
  } else {
    const char *iife_end = "})()";
    memcpy(output + output_pos, iife_end, strlen(iife_end));
    output_pos += strlen(iife_end);
  }
  
  for (int i = 0; i < export_count; i++) {
    free(exports[i]);
  }
  free(exports);
  
  output[output_pos] = '\0';
  *output_len = output_pos;
  return output;
}

static char *process_snapshot_includes(const char *file_path, const char *content, size_t content_len, module_cache_t *cache, size_t *output_len) {
  size_t output_capacity = content_len * 2;
  char *output = malloc(output_capacity);
  if (!output) return NULL;
  
  size_t output_pos = 0;
  const char *pos = content;
  const char *end = content + content_len;
  
  while (pos < end) {
    const char *include_start = strstr(pos, "snapshot_include(");
    if (!include_start || include_start >= end) {
      size_t remaining = end - pos;
      if (output_pos + remaining >= output_capacity) {
        output_capacity = (output_pos + remaining) * 2;
        output = realloc(output, output_capacity);
      }
      memcpy(output + output_pos, pos, remaining);
      output_pos += remaining;
      break;
    }
    
    memcpy(output + output_pos, pos, include_start - pos);
    output_pos += include_start - pos;
    
    const char *quote_start = strchr(include_start, '\'');
    if (!quote_start) quote_start = strchr(include_start, '"');
    if (!quote_start) {
      pos = include_start + 17;
      continue;
    }
    
    char quote_char = *quote_start;
    const char *quote_end = strchr(quote_start + 1, quote_char);
    if (!quote_end) {
      pos = include_start + 17;
      continue;
    }
    
    size_t path_len = quote_end - quote_start - 1;
    char *import_path = malloc(path_len + 1);
    memcpy(import_path, quote_start + 1, path_len);
    import_path[path_len] = '\0';
    
    char *resolved_path = resolve_path(file_path, import_path);
    free(import_path);
    
    if (!resolved_path) {
      pos = include_start + 17;
      continue;
    }
    
    if (module_already_processed(cache, resolved_path)) {
      fprintf(stderr, "Warning: Circular dependency detected: %s\n", resolved_path);
      free(resolved_path);
      pos = quote_end + 2;
      continue;
    }
    
    add_to_cache(cache, resolved_path);
    
    size_t module_len;
    char *module_content = read_file(resolved_path, &module_len);
    
    if (!module_content) {
      fprintf(stderr, "Error: Cannot read module: %s\n", resolved_path);
      free(resolved_path);
      free(output);
      return NULL;
    }
    
    size_t processed_len;
    char *processed = process_snapshot_includes(resolved_path, module_content, module_len, cache, &processed_len);
    free(module_content);
    
    if (!processed) {
      free(resolved_path);
      free(output);
      return NULL;
    }
    
    size_t wrapped_len;
    char *wrapped = wrap_module(processed, processed_len, &wrapped_len);
    free(processed);
    free(resolved_path);
    
    if (!wrapped) {
      free(output);
      return NULL;
    }
    
    if (output_pos + wrapped_len >= output_capacity) {
      output_capacity = (output_pos + wrapped_len) * 2;
      output = realloc(output, output_capacity);
    }
    
    memcpy(output + output_pos, wrapped, wrapped_len);
    output_pos += wrapped_len;
    free(wrapped);
    
    const char *paren_close = strchr(quote_end, ')');
    if (paren_close) {
      pos = paren_close + 1;
    } else {
      pos = quote_end + 1;
    }
  }
  
  output[output_pos] = '\0';
  *output_len = output_pos;
  return output;
}

static char *replace_templates(const char *input, size_t input_len, replacement_t *replacements, int num_replacements, size_t *output_len) {
  size_t output_size = input_len;
  
  for (int i = 0; i < num_replacements; i++) {
    const char *placeholder = replacements[i].placeholder;
    const char *value = replacements[i].value;
    size_t placeholder_len = strlen(placeholder);
    size_t value_len = strlen(value);
    
    const char *pos = input;
    while ((pos = strstr(pos, placeholder)) != NULL) {
      output_size = output_size - placeholder_len + value_len;
      pos += placeholder_len;
    }
  }

  char *output = malloc(output_size + 1);
  if (!output) return NULL;
  
  const char *read_pos = input;
  char *write_pos = output;
  size_t remaining = input_len;
  
  while (remaining > 0) {
    const char *nearest_match = NULL;
    size_t nearest_match_len = 0;
    const char *nearest_match_value = NULL;
    
    for (int i = 0; i < num_replacements; i++) {
      const char *match = strstr(read_pos, replacements[i].placeholder);
      if (match && (!nearest_match || match < nearest_match)) {
        nearest_match = match;
        nearest_match_len = strlen(replacements[i].placeholder);
        nearest_match_value = replacements[i].value;
      }
    }
    
    if (nearest_match) {
      size_t before_len = nearest_match - read_pos;
      memcpy(write_pos, read_pos, before_len);
      write_pos += before_len;
      
      size_t value_len = strlen(nearest_match_value);
      memcpy(write_pos, nearest_match_value, value_len);
      write_pos += value_len;
      
      read_pos = nearest_match + nearest_match_len;
      remaining = input_len - (read_pos - input);
    } else {
      memcpy(write_pos, read_pos, remaining);
      write_pos += remaining;
      remaining = 0;
    }
  }
  
  *write_pos = '\0';
  *output_len = write_pos - output;
  return output;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "Usage: %s <input.js> <output.h> [KEY=value...]\n", argv[0]);
    fprintf(stderr, "Example: %s core.js snapshot.h VERSION=1.0.0 GIT_HASH=abc123\n", argv[0]);
    return 1;
  }

  const char *input_file = argv[1];
  const char *output_file = argv[2];
  
  int num_replacements = argc - 3;
  replacement_t *replacements = malloc(sizeof(replacement_t) * num_replacements);
  if (!replacements && num_replacements > 0) {
    fprintf(stderr, "Error: Failed to allocate memory for replacements\n");
    return 1;
  }
  
  int replacement_idx = 0;
  for (int i = 3; i < argc; i++) {
    char *arg = strdup(argv[i]);
    char *equals = strchr(arg, '=');
    if (equals) {
      *equals = '\0';
      
      replacements[replacement_idx].placeholder = malloc(strlen(arg) + 5);
      sprintf(replacements[replacement_idx].placeholder, "{{%s}}", arg);
      replacements[replacement_idx].value = strdup(equals + 1);
      
      printf("template replacement: %s -> %s\n", replacements[replacement_idx].placeholder, replacements[replacement_idx].value);
      replacement_idx++;
    }
    free(arg);
  }
  
  num_replacements = replacement_idx;

  FILE *in = fopen(input_file, "r");
  if (!in) {
    fprintf(stderr, "Error: Cannot open input file: %s\n", input_file);
    return 1;
  }

  fseek(in, 0, SEEK_END);
  long file_size = ftell(in);
  fseek(in, 0, SEEK_SET);

  char *js_code_original = malloc(file_size + 1);
  if (!js_code_original) {
    fprintf(stderr, "Error: Memory allocation failed\n");
    fclose(in);
    return 1;
  }

  fread(js_code_original, 1, file_size, in);
  js_code_original[file_size] = '\0';
  fclose(in);
  
  module_cache_t cache = {0};
  
  size_t bundled_len;
  char *bundled_code = process_snapshot_includes(input_file, js_code_original, file_size, &cache, &bundled_len);
  
  if (!bundled_code) {
    fprintf(stderr, "Error: Module bundling failed\n");
    free(js_code_original);
    return 1;
  }
  
  size_t processed_len;
  char *js_code = replace_templates(bundled_code, bundled_len, replacements, num_replacements, &processed_len);
  
  if (!js_code) {
    fprintf(stderr, "Error: Template replacement failed\n");
    free(js_code_original);
    free(bundled_code);
    return 1;
  }
  
  free(js_code_original);
  free(bundled_code);
  
  char *compacted = malloc(processed_len + 1);
  if (!compacted) {
    fprintf(stderr, "Error: Memory allocation failed for compaction\n");
    free(js_code);
    return 1;
  }
  
  size_t compact_pos = 0;
  for (size_t i = 0; i < processed_len; i++) {
    if (js_code[i] != '\n' && js_code[i] != '\r') {
      compacted[compact_pos++] = js_code[i];
    }
  }
  compacted[compact_pos] = '\0';
  
  free(js_code);
  js_code = compacted;
  processed_len = compact_pos;
  
  for (int i = 0; i < cache.count; i++) {
    free(cache.paths[i]);
  }
  free(cache.paths);

  FILE *out = fopen(output_file, "w");
  if (!out) {
    fprintf(stderr, "Error: Cannot open output file: %s\n", output_file);
    free(js_code);
    return 1;
  }

  fprintf(out, "/* Auto-generated snapshot from %s */\n", input_file);
  fprintf(out, "/* DO NOT EDIT - Generated during build */\n\n");
  fprintf(out, "#ifndef ANT_SNAPSHOT_DATA_H\n");
  fprintf(out, "#define ANT_SNAPSHOT_DATA_H\n\n");
  fprintf(out, "#include <stddef.h>\n");
  fprintf(out, "#include <stdint.h>\n\n");
  
  fprintf(out, "static const uint8_t ant_snapshot_source[] = {");
  
  for (size_t i = 0; i < processed_len; i++) {
    if (i % 16 == 0) {
      fprintf(out, "\n  ");
    }
    fprintf(out, "0x%02x", (unsigned char)js_code[i]);
    if (i < processed_len - 1) {
      fprintf(out, ", ");
    }
  }
  
  fprintf(out, "\n};\n\n");
  fprintf(out, "static const size_t ant_snapshot_source_len = %zu;\n\n", processed_len);
  fprintf(out, "/* bundled source size: %zu bytes */\n", processed_len);
  fprintf(out, "static const size_t ant_snapshot_mem_size = %zu;\n\n", processed_len);
  
  fprintf(out, "#endif /* ANT_SNAPSHOT_DATA_H */\n");

  fclose(out);
  free(js_code);
  
  for (int i = 0; i < num_replacements; i++) {
    free(replacements[i].placeholder);
    free(replacements[i].value);
  }
  free(replacements);

  printf("snapshot generated successfully: %s\n", output_file);
  printf("  original size: %ld bytes\n", file_size);
  printf("  bundled size: %zu bytes\n", processed_len);
  printf("  replacements: %d\n", num_replacements);

  return 0;
}
