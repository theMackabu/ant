#include "readline.h"
#include "utils.h"
#include "readline_internal.h" // IWYU pragma: keep

static const unsigned char HISTORY_FILE_MAGIC[] = {
  0x61, 0x6E, 0x74, 0x72, 0x65, 0x70, 0x6C, 0x73,
  0x63, 0x68, 0x65, 0x6D, 0x61, 0x32, 0x0A, 0x00
};

void ant_history_init(ant_history_t *hist, int capacity) {
  hist->capacity = (capacity > 0) ? capacity : 512;
  hist->lines = malloc(sizeof(char *) * (size_t)hist->capacity);
  if (!hist->lines) hist->capacity = 0;
  hist->count = 0;
  hist->current = -1;
}

void ant_history_add(ant_history_t *hist, const char *line) {
  if (!hist || !hist->lines || hist->capacity <= 0 || !line || line[0] == '\0') return;

  if (hist->count > 0 && strcmp(hist->lines[hist->count - 1], line) == 0) {
    hist->current = hist->count;
    return;
  }

  if (hist->count >= hist->capacity) {
    free(hist->lines[0]);
    memmove(hist->lines, hist->lines + 1, sizeof(char *) * (size_t)(hist->capacity - 1));
    hist->count--;
  }

  hist->lines[hist->count++] = strdup(line);
  hist->current = hist->count;
}

const char *ant_history_prev(ant_history_t *hist) {
  if (!hist || !hist->lines || hist->count == 0) return NULL;
  if (hist->current > 0) hist->current--;
  return hist->lines[hist->current];
}

const char *ant_history_next(ant_history_t *hist) {
  if (!hist || !hist->lines || hist->count == 0) return NULL;
  if (hist->current < hist->count - 1) {
    hist->current++;
    return hist->lines[hist->current];
  }
  hist->current = hist->count;
  return "";
}

void ant_history_free(ant_history_t *hist) {
  if (!hist || !hist->lines) return;
  for (int i = 0; i < hist->count; i++) free(hist->lines[i]);
  free(hist->lines);
  hist->lines = NULL;
  hist->count = 0;
  hist->capacity = 0;
  hist->current = -1;
}

static char *get_history_path(void) {
  char dir[4096];

  if (ant_xdg_state_path(dir, sizeof(dir), NULL) != 0) return NULL;
  if (ant_mkdir_p(dir) != 0) return NULL;

  size_t len = strlen(dir) + sizeof("/repl_history");
  char *path = malloc(len);

  if (!path) return NULL;
  snprintf(path, len, "%s/repl_history", dir);

  return path;
}

void ant_history_load(ant_history_t *hist) {
  if (!hist || !hist->lines || hist->capacity <= 0) return;

  char *path = get_history_path();
  if (!path) return;

  FILE *fp = fopen(path, "r");
  free(path);

  if (!fp) return;
  char header[sizeof(HISTORY_FILE_MAGIC)];

  if (
    !fgets(header, sizeof(header), fp) ||
    strcmp(header, (const char *)HISTORY_FILE_MAGIC) != 0
  ) {
    fclose(fp);
    return;
  }

  char lenbuf[32];
  while (fgets(lenbuf, sizeof(lenbuf), fp)) {
    char *end = NULL;
    unsigned long long record_len = strtoull(lenbuf, &end, 10);

    if (end == lenbuf || (*end != '\n' && *end != '\0')) break;
    if (record_len > (unsigned long long)SIZE_MAX - 1) break;

    size_t line_len = (size_t)record_len;
    char *line = malloc(line_len + 1);

    if (!line) break;

    if (fread(line, 1, line_len, fp) != line_len) {
      free(line);
      break;
    }

    line[line_len] = '\0';
    if (line[0]) ant_history_add(hist, line);
    free(line);

    int sep = fgetc(fp);
    if (sep != '\n' && sep != EOF) break;
  }

  fclose(fp);
}

void ant_history_save(const ant_history_t *hist) {
  if (!hist || !hist->lines) return;

  char *path = get_history_path();
  if (!path) return;

  FILE *fp = fopen(path, "w");
  free(path);

  if (!fp) return;
  fputs((const char *)HISTORY_FILE_MAGIC, fp);

  for (int i = 0; i < hist->count; i++) {
    size_t len = strlen(hist->lines[i]);
    fprintf(fp, "%zu\n", len);
    fwrite(hist->lines[i], 1, len, fp);
    fputc('\n', fp);
  }

  fclose(fp);
}
