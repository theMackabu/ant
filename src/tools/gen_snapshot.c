#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ant.h"
#include "runtime.h"

typedef struct {
  char *placeholder;
  char *value;
} replacement_t;

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
  
  size_t processed_len;
  char *js_code = replace_templates(js_code_original, file_size, replacements, num_replacements, &processed_len);
  
  if (!js_code) {
    fprintf(stderr, "Error: Template replacement failed\n");
    free(js_code_original);
    return 1;
  }
  
  free(js_code_original);

  struct js *js = js_create_dynamic(1024 * 1024, 10 * 1024 * 1024);
  if (!js) {
    fprintf(stderr, "Error: Failed to create JS runtime\n");
    free(js_code);
    return 1;
  }

  ant_runtime_init(js);

  jsval_t result = js_eval(js, js_code, processed_len);
  if (js_type(result) == JS_ERR) {
    fprintf(stderr, "Error: Failed to evaluate JS code:\n%s\n", js_str(js, result));
    js_destroy(js);
    free(js_code);
    return 1;
  }

  size_t total_mem, min_free, cstack;
  js_stats(js, &total_mem, &min_free, &cstack);
  size_t used_mem = js_getbrk(js);

  FILE *out = fopen(output_file, "w");
  if (!out) {
    fprintf(stderr, "Error: Cannot open output file: %s\n", output_file);
    js_destroy(js);
    free(js_code);
    return 1;
  }

  fprintf(out, "/* Auto-generated snapshot from %s */\n", input_file);
  fprintf(out, "/* DO NOT EDIT - Generated during build */\n\n");
  fprintf(out, "#ifndef ANT_SNAPSHOT_DATA_H\n");
  fprintf(out, "#define ANT_SNAPSHOT_DATA_H\n\n");
  fprintf(out, "#include <stddef.h>\n\n");
  
  fprintf(out, "static const char ant_snapshot_source[] = \n");
  fprintf(out, "\"");
  
  for (size_t i = 0; i < processed_len; i++) {
    char c = js_code[i];
    switch (c) {
      case '\n': fprintf(out, "\\n"); break;
      case '\r': fprintf(out, "\\r"); break;
      case '\t': fprintf(out, "\\t"); break;
      case '\\': fprintf(out, "\\\\"); break;
      case '"':  fprintf(out, "\\\""); break;
      default:
        if (c >= 32 && c < 127) {
          fputc(c, out);
        } else {
          fprintf(out, "\\x%02x", (unsigned char)c);
        }
        break;
    }
    
    if (i > 0 && i % 80 == 0) fprintf(out, "\"\n\"");
  }
  
  fprintf(out, "\";\n\n");
  fprintf(out, "static const size_t ant_snapshot_source_len = %zu;\n\n", processed_len);
  fprintf(out, "/* memory usage after evaluation: %zu bytes */\n", used_mem);
  fprintf(out, "/* total memory: %zu bytes */\n", total_mem);
  fprintf(out, "static const size_t ant_snapshot_mem_size = %zu;\n\n", used_mem);
  
  fprintf(out, "#endif /* ANT_SNAPSHOT_DATA_H */\n");

  fclose(out);
  js_destroy(js);
  free(js_code);
  
  for (int i = 0; i < num_replacements; i++) {
    free(replacements[i].placeholder);
    free(replacements[i].value);
  }
  free(replacements);

  printf("snapshot generated successfully: %s\n", output_file);
  printf("  original size: %ld bytes\n", file_size);
  printf("  processed size: %zu bytes\n", processed_len);
  printf("  memory used: %zu bytes\n", used_mem);
  printf("  replacements: %d\n", num_replacements);

  return 0;
}
