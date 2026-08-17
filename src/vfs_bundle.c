#include "vfs_bundle.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

typedef struct {
  char magic[8];
  uint32_t format_version;
  uint32_t reserved;
  uint64_t payload_offset;
  uint64_t payload_size;
} ant_bundle_footer_t;

typedef struct {
  char abi_hash[ANT_BUNDLE_ABI_HASH_MAX];
  uint32_t module_count;
  uint32_t edge_count;
  uint32_t entry_idx;
  uint32_t reserved;
  uint64_t strtab_off;
  uint64_t strtab_len;
  uint64_t data_off;
} ant_bundle_header_t;

typedef struct {
  uint32_t key_stroff;
  uint8_t format;
  uint8_t kind;
  uint16_t reserved;
  uint64_t data_off;
  uint64_t data_len;
} ant_bundle_mod_rec_t;

typedef struct ant_bundle_edge_rec {
  uint32_t parent_idx;
  uint32_t spec_stroff;
  uint32_t child_idx;
  uint32_t flags;
} ant_bundle_edge_rec_t;

static_assert(sizeof(ant_bundle_footer_t) == 32, "footer layout");
static_assert(sizeof(ant_bundle_header_t) == ANT_BUNDLE_ABI_HASH_MAX + 40, "header layout");
static_assert(sizeof(ant_bundle_mod_rec_t) == 24, "module record layout");
static_assert(sizeof(ant_bundle_edge_rec_t) == 16, "edge record layout");

#define ANT_BUNDLE_EDGE_REQUIRE 0x1u

static uint64_t align8(uint64_t n) {
  return (n + 7u) & ~7ull;
}

typedef struct {
  char *buf;
  size_t len;
  size_t cap;
} strtab_builder_t;

static int64_t strtab_add(strtab_builder_t *st, const char *s) {
  size_t n = strlen(s) + 1;
  if (st->len + n > st->cap) {
    size_t cap = st->cap ? st->cap * 2 : 256;
    while (cap < st->len + n) cap *= 2;
    char *next = realloc(st->buf, cap);
    if (!next) return -1;
    st->buf = next;
    st->cap = cap;
  }
  memcpy(st->buf + st->len, s, n);
  int64_t off = (int64_t)st->len;
  st->len += n;
  return off;
}

typedef struct {
  const char *strtab;
  const ant_bundle_edge_rec_t *recs;
} edge_sort_ctx_t;

static edge_sort_ctx_t edge_sort_ctx;

static int edge_rec_cmp(const void *a, const void *b) {
  const ant_bundle_edge_rec_t *ea = a, *eb = b;
  if (ea->parent_idx != eb->parent_idx) return ea->parent_idx < eb->parent_idx ? -1 : 1;
  int c = strcmp(edge_sort_ctx.strtab + ea->spec_stroff, edge_sort_ctx.strtab + eb->spec_stroff);
  if (c) return c;
  return (int)(ea->flags & ANT_BUNDLE_EDGE_REQUIRE) - (int)(eb->flags & ANT_BUNDLE_EDGE_REQUIRE);
}

static int fwrite_padded(FILE *f, const void *data, size_t len, size_t padded_len) {
  static const char zeros[8] = {0};
  if (len && fwrite(data, 1, len, f) != len) return -1;
  size_t pad = padded_len - len;
  if (pad && fwrite(zeros, 1, pad, f) != pad) return -1;
  return 0;
}

int ant_bundle_write(FILE *f, const ant_bundle_build_t *build) {
  int rc = -1;
  ant_bundle_mod_rec_t *mods = NULL;
  ant_bundle_edge_rec_t *edges = NULL;
  strtab_builder_t st = {0};

  if (!build->module_count || build->entry_idx >= build->module_count) return -1;

  mods = calloc(build->module_count, sizeof(*mods));
  edges = build->edge_count ? calloc(build->edge_count, sizeof(*edges)) : NULL;
  if (!mods || (build->edge_count && !edges)) goto done;

  for (uint32_t i = 0; i < build->module_count; i++) {
    int64_t off = strtab_add(&st, build->modules[i].key);
    if (off < 0) goto done;
    mods[i].key_stroff = (uint32_t)off;
    mods[i].format = build->modules[i].format;
    mods[i].kind = build->modules[i].kind;
  }

  for (uint32_t i = 0; i < build->edge_count; i++) {
    const ant_bundle_build_edge_t *e = &build->edges[i];
    if (e->parent_idx >= build->module_count || e->child_idx >= build->module_count) goto done;
    int64_t off = strtab_add(&st, e->spec);
    if (off < 0) goto done;
    edges[i].parent_idx = e->parent_idx;
    edges[i].spec_stroff = (uint32_t)off;
    edges[i].child_idx = e->child_idx;
    edges[i].flags = e->is_require ? ANT_BUNDLE_EDGE_REQUIRE : 0;
  }

  edge_sort_ctx = (edge_sort_ctx_t){ st.buf, edges };
  if (build->edge_count) qsort(edges, build->edge_count, sizeof(*edges), edge_rec_cmp);

  ant_bundle_header_t hdr = {0};
  const char *abi = build->abi_hash ? build->abi_hash : "";
  snprintf(hdr.abi_hash, sizeof(hdr.abi_hash), "%s", abi);
  hdr.module_count = build->module_count;
  hdr.edge_count = build->edge_count;
  hdr.entry_idx = build->entry_idx;
  hdr.strtab_off = sizeof(hdr)
    + (uint64_t)build->module_count * sizeof(ant_bundle_mod_rec_t)
    + (uint64_t)build->edge_count * sizeof(ant_bundle_edge_rec_t);
  hdr.strtab_len = st.len;
  hdr.data_off = align8(hdr.strtab_off + hdr.strtab_len);

  uint64_t data_cursor = hdr.data_off;
  for (uint32_t i = 0; i < build->module_count; i++) {
    mods[i].data_off = data_cursor;
    mods[i].data_len = build->modules[i].data_len;
    data_cursor = align8(data_cursor + mods[i].data_len + 1);
  }
  uint64_t payload_size = data_cursor;

  if (fseek(f, 0, SEEK_END) != 0) goto done;
  long end = ftell(f);
  if (end < 0) goto done;

  uint64_t payload_offset = align8((uint64_t)end);
  if (fwrite_padded(f, NULL, 0, (size_t)(payload_offset - (uint64_t)end)) != 0) goto done;

  if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) goto done;
  if (build->module_count && fwrite(mods, sizeof(*mods), build->module_count, f) != build->module_count) goto done;
  if (build->edge_count && fwrite(edges, sizeof(*edges), build->edge_count, f) != build->edge_count) goto done;
  if (fwrite_padded(f, st.buf, st.len, (size_t)(hdr.data_off - hdr.strtab_off)) != 0) goto done;

  for (uint32_t i = 0; i < build->module_count; i++) {
    uint64_t next = i + 1 < build->module_count ? mods[i + 1].data_off : payload_size;
    if (fwrite_padded(f, build->modules[i].data, (size_t)mods[i].data_len, (size_t)(next - mods[i].data_off)) != 0) goto done;
  }

  ant_bundle_footer_t footer = {0};
  memcpy(footer.magic, ANT_BUNDLE_MAGIC, sizeof(footer.magic));
  footer.format_version = ANT_BUNDLE_FORMAT_VERSION;
  footer.payload_offset = payload_offset;
  footer.payload_size = payload_size;
  if (fwrite(&footer, 1, sizeof(footer), f) != sizeof(footer)) goto done;

  rc = 0;

done:
  free(mods);
  free(edges);
  free(st.buf);
  return rc;
}

static bool strtab_ref_ok(const ant_bundle_t *b, uint64_t off) {
  return off < b->strtab_len;
}

static bool footer_valid(const ant_bundle_footer_t *footer, uint64_t footer_off) {
  if (memcmp(footer->magic, ANT_BUNDLE_MAGIC, sizeof(footer->magic)) != 0) return false;
  if (footer->format_version != ANT_BUNDLE_FORMAT_VERSION) return false;
  if (footer->payload_size < sizeof(ant_bundle_header_t)) return false;
  if (footer->payload_offset % 8u != 0) return false;
  if (footer->payload_offset > footer_off ||
      footer->payload_size > footer_off ||
      footer->payload_offset + footer->payload_size > footer_off) return false;
  return true;
}

#define ANT_BUNDLE_SCAN_MAX (4u << 20)

static bool find_footer(FILE *f, uint64_t file_size, ant_bundle_footer_t *out) {
  if (fseek(f, (long)(file_size - sizeof(*out)), SEEK_SET) == 0 &&
      fread(out, 1, sizeof(*out), f) == sizeof(*out) &&
      footer_valid(out, file_size - sizeof(*out))) {
    return true;
  }

  uint64_t scan_len = file_size < ANT_BUNDLE_SCAN_MAX ? file_size : ANT_BUNDLE_SCAN_MAX;
  uint64_t scan_start = file_size - scan_len;
  uint8_t *tail = malloc((size_t)scan_len);
  if (!tail) return false;

  if (fseek(f, (long)scan_start, SEEK_SET) != 0 ||
      fread(tail, 1, (size_t)scan_len, f) != (size_t)scan_len) {
    free(tail);
    return false;
  }

  for (int64_t off = (int64_t)scan_len - (int64_t)sizeof(*out); off >= 0; off--) {
    if (memcmp(tail + off, ANT_BUNDLE_MAGIC, 8) != 0) continue;
    ant_bundle_footer_t candidate;
    memcpy(&candidate, tail + off, sizeof(candidate));
    if (footer_valid(&candidate, scan_start + (uint64_t)off)) {
      *out = candidate;
      free(tail);
      return true;
    }
  }

  free(tail);
  return false;
}

ant_bundle_status_t ant_bundle_open(const char *exe_path, const char *expected_abi, ant_bundle_t *out) {
  memset(out, 0, sizeof(*out));

  FILE *f = fopen(exe_path, "rb");
  if (!f) return ANT_BUNDLE_ERR_IO;

  ant_bundle_status_t status = ANT_BUNDLE_ERR_CORRUPT;
  ant_bundle_footer_t footer;

  if (fseek(f, 0, SEEK_END) != 0) { status = ANT_BUNDLE_ERR_IO; goto fail; }
  long file_size = ftell(f);
  if (file_size < (long)sizeof(footer)) { status = ANT_BUNDLE_ERR_NO_TRAILER; goto fail; }

  if (!find_footer(f, (uint64_t)file_size, &footer)) {
    status = ANT_BUNDLE_ERR_NO_TRAILER;
    goto fail;
  }

  out->payload = malloc((size_t)footer.payload_size);
  if (!out->payload) { status = ANT_BUNDLE_ERR_OOM; goto fail; }
  out->payload_size = (size_t)footer.payload_size;

  if (fseek(f, (long)footer.payload_offset, SEEK_SET) != 0 ||
      fread(out->payload, 1, out->payload_size, f) != out->payload_size) {
    status = ANT_BUNDLE_ERR_IO;
    goto fail;
  }
  fclose(f);
  f = NULL;

  ant_bundle_header_t hdr;
  memcpy(&hdr, out->payload, sizeof(hdr));
  memcpy(out->abi_hash, hdr.abi_hash, sizeof(out->abi_hash));
  out->abi_hash[sizeof(out->abi_hash) - 1] = '\0';

  if (expected_abi && strncmp(out->abi_hash, expected_abi, sizeof(out->abi_hash)) != 0) {
    char found[ANT_BUNDLE_ABI_HASH_MAX];
    memcpy(found, out->abi_hash, sizeof(found));
    ant_bundle_close(out);
    memcpy(out->abi_hash, found, sizeof(out->abi_hash));
    return ANT_BUNDLE_ERR_ABI;
  }

  uint64_t mods_off = sizeof(hdr);
  uint64_t mods_len = (uint64_t)hdr.module_count * sizeof(ant_bundle_mod_rec_t);
  uint64_t edges_off = mods_off + mods_len;
  uint64_t edges_len = (uint64_t)hdr.edge_count * sizeof(ant_bundle_edge_rec_t);

  if (hdr.module_count == 0 || hdr.entry_idx >= hdr.module_count) goto fail;
  if (mods_len / sizeof(ant_bundle_mod_rec_t) != hdr.module_count) goto fail;
  if (edges_len / sizeof(ant_bundle_edge_rec_t) != (uint64_t)hdr.edge_count && hdr.edge_count) goto fail;
  if (hdr.strtab_off != edges_off + edges_len) goto fail;
  if (hdr.strtab_len == 0 || hdr.strtab_len > out->payload_size ||
      hdr.strtab_off + hdr.strtab_len > out->payload_size) goto fail;
  if (hdr.data_off < hdr.strtab_off + hdr.strtab_len || hdr.data_off > out->payload_size) goto fail;

  out->strtab = (const char *)out->payload + hdr.strtab_off;
  out->strtab_len = hdr.strtab_len;
  if (out->strtab[out->strtab_len - 1] != '\0') goto fail;

  out->module_count = hdr.module_count;
  out->edge_count = hdr.edge_count;
  out->entry_idx = hdr.entry_idx;
  out->edges = (const ant_bundle_edge_rec_t *)(out->payload + edges_off);

  out->modules = calloc(hdr.module_count, sizeof(*out->modules));
  if (!out->modules) { status = ANT_BUNDLE_ERR_OOM; goto fail; }

  const ant_bundle_mod_rec_t *recs = (const ant_bundle_mod_rec_t *)(out->payload + mods_off);
  for (uint32_t i = 0; i < hdr.module_count; i++) {
    if (!strtab_ref_ok(out, recs[i].key_stroff)) goto fail;
    if (recs[i].data_off < hdr.data_off ||
        recs[i].data_off > out->payload_size ||
        recs[i].data_len >= out->payload_size - recs[i].data_off) goto fail;
    if (out->payload[recs[i].data_off + recs[i].data_len] != '\0') goto fail;

    out->modules[i].key = out->strtab + recs[i].key_stroff;
    out->modules[i].format = recs[i].format;
    out->modules[i].kind = recs[i].kind;
    out->modules[i].data = out->payload + recs[i].data_off;
    out->modules[i].data_len = recs[i].data_len;
  }

  for (uint32_t i = 0; i < hdr.edge_count; i++) {
    if (out->edges[i].parent_idx >= hdr.module_count ||
        out->edges[i].child_idx >= hdr.module_count ||
        !strtab_ref_ok(out, out->edges[i].spec_stroff)) goto fail;
  }

  return ANT_BUNDLE_OK;

fail:
  if (f) fclose(f);
  ant_bundle_close(out);
  return status;
}

void ant_bundle_close(ant_bundle_t *bundle) {
  free(bundle->modules);
  free(bundle->payload);
  memset(bundle, 0, sizeof(*bundle));
}

const ant_bundle_module_t *ant_bundle_entry(const ant_bundle_t *bundle) {
  return &bundle->modules[bundle->entry_idx];
}

static int bundle_find(const ant_bundle_t *bundle, const char *key) {
  for (uint32_t i = 0; i < bundle->module_count; i++) {
    if (strcmp(bundle->modules[i].key, key) == 0) return (int)i;
  }
  return -1;
}

const ant_bundle_module_t *ant_bundle_get(const ant_bundle_t *bundle, const char *key) {
  int idx = bundle_find(bundle, key);
  return idx < 0 ? NULL : &bundle->modules[idx];
}

bool ant_bundle_has_key(const ant_bundle_t *bundle, const char *key) {
  return bundle_find(bundle, key) >= 0;
}

const char *ant_bundle_resolve(const ant_bundle_t *bundle, const char *parent_key, const char *spec, bool is_require) {
  if (!parent_key || !spec) return NULL;
  int parent = bundle_find(bundle, parent_key);
  if (parent < 0) return NULL;

  uint32_t lo = 0, hi = bundle->edge_count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    const struct ant_bundle_edge_rec *e = &bundle->edges[mid];
    int c = e->parent_idx == (uint32_t)parent
      ? strcmp(bundle->strtab + e->spec_stroff, spec)
      : (e->parent_idx < (uint32_t)parent ? -1 : 1);
    if (c < 0) lo = mid + 1;
    else hi = mid;
  }

  const struct ant_bundle_edge_rec *fallback = NULL;
  for (uint32_t i = lo; i < bundle->edge_count; i++) {
    const struct ant_bundle_edge_rec *e = &bundle->edges[i];
    if (e->parent_idx != (uint32_t)parent || strcmp(bundle->strtab + e->spec_stroff, spec) != 0) break;
    bool edge_require = (e->flags & ANT_BUNDLE_EDGE_REQUIRE) != 0;
    if (edge_require == is_require) return bundle->modules[e->child_idx].key;
    fallback = e;
  }
  return fallback ? bundle->modules[fallback->child_idx].key : NULL;
}

const char *ant_bundle_status_str(ant_bundle_status_t status) {
  switch (status) {
    case ANT_BUNDLE_OK: return "ok";
    case ANT_BUNDLE_ERR_IO: return "I/O error reading executable";
    case ANT_BUNDLE_ERR_NO_TRAILER: return "no embedded program";
    case ANT_BUNDLE_ERR_CORRUPT: return "embedded program data is corrupt";
    case ANT_BUNDLE_ERR_ABI: return "embedded program was built for a different ant revision";
    case ANT_BUNDLE_ERR_OOM: return "out of memory";
  }
  return "unknown error";
}
