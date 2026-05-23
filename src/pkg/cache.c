#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define mkdir(path, mode) _mkdir(path)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#else
#include <dirent.h>
#include <unistd.h>
#endif

#define MAP_SIZE ((size_t)8 * 1024 * 1024 * 1024)
#define METADATA_TTL_SECS ((int64_t)24 * 60 * 60)
#define BLOCK_SIZE 4096

// External metadata strip functions from metadata.c
extern char *strip_npm_metadata(const char *json_data, size_t json_len,
                                size_t *out_len);
extern void strip_metadata_free(char *ptr);

static bool make_dir_recursive(const char *path) {
  char temp[4096];
  snprintf(temp, sizeof(temp), "%s", path);
  size_t len = strlen(temp);
  if (len == 0)
    return false;

  for (size_t i = 1; i < len; i++) {
    if (temp[i] == '/' || temp[i] == '\\') {
      char c = temp[i];
      temp[i] = '\0';
      mkdir(temp, 0755);
      temp[i] = c;
    }
  }
  return mkdir(temp, 0755) == 0 || errno == EEXIST;
}

void cache_delete_tree(const char *path) {
#ifdef _WIN32
  // Simplified for Windows using system command or path scan
  char cmd[4096];
  snprintf(cmd, sizeof(cmd), "rmdir /s /q \"%s\"", path);
  system(cmd);
#else
  DIR *d = opendir(path);
  if (!d)
    return;

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char subpath[4096];
    snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);

    struct stat st;
    if (lstat(subpath, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        cache_delete_tree(subpath);
      } else {
        unlink(subpath);
      }
    }
  }
  closedir(d);
  rmdir(path);
#endif
}

static size_t align_to_block(uint64_t size) {
  return ((size + BLOCK_SIZE - 1) / BLOCK_SIZE) * BLOCK_SIZE;
}

static size_t get_dir_size_recursive(const char *path) {
  size_t total = 0;
#ifdef _WIN32
  // Basic implementation for Windows
  struct _finddata_t data;
  char pattern[4096];
  snprintf(pattern, sizeof(pattern), "%s\\*", path);
  intptr_t handle = _findfirst(pattern, &data);
  if (handle != -1) {
    do {
      if (strcmp(data.name, ".") == 0 || strcmp(data.name, "..") == 0)
        continue;
      char subpath[4096];
      snprintf(subpath, sizeof(subpath), "%s/%s", path, data.name);
      if (data.attrib & _A_SUBDIR) {
        total += get_dir_size_recursive(subpath);
      } else {
        total += align_to_block(data.size);
      }
    } while (_findnext(handle, &data) == 0);
    _findclose(handle);
  }
#else
  DIR *d = opendir(path);
  if (!d)
    return 0;

  struct dirent *entry;
  while ((entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;

    char subpath[4096];
    snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);

    struct stat st;
    if (lstat(subpath, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        total += get_dir_size_recursive(subpath);
      } else {
        total += align_to_block(st.st_size);
      }
    }
  }
  closedir(d);
#endif
  return total;
}

static void make_integrity_key(const uint8_t integrity[64],
                               uint8_t out_key[66]) {
  out_key[0] = 'i';
  out_key[1] = ':';
  memcpy(out_key + 2, integrity, 64);
}

static char *make_name_key(const char *name, const char *version) {
  size_t len = strlen(name) + strlen(version) + 4;
  char *key = malloc(len);
  if (!key)
    return NULL;
  snprintf(key, len, "n:%s@%s", name, version);
  return key;
}

static char *make_metadata_key(const char *name) {
  size_t len = strlen(name) + 3;
  char *key = malloc(len);
  if (!key)
    return NULL;
  snprintf(key, len, "m:%s", name);
  return key;
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out_hex) {
  for (size_t i = 0; i < len; i++) {
    sprintf(out_hex + (i * 2), "%02x", bytes[i]);
  }
  out_hex[len * 2] = '\0';
}

static void prune_expired_metadata_txn(cache_db_t *db, MDB_txn *txn) {
  MDB_cursor *cursor = NULL;
  if (mdb_cursor_open(txn, db->dbi_metadata, &cursor) != 0)
    return;

  MDB_val key, val;
  int rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST);
  int64_t now = (int64_t)time(NULL);

  while (rc == 0) {
    if (val.mv_size >= sizeof(int64_t)) {
      int64_t cached_at;
      memcpy(&cached_at, val.mv_data, sizeof(int64_t));
      if (now - cached_at > METADATA_TTL_SECS) {
        // Expired, delete
        mdb_cursor_del(cursor, 0);
      }
    }
    rc = mdb_cursor_get(cursor, &key, &val, MDB_NEXT);
  }
  mdb_cursor_close(cursor);
}

cache_db_t *cache_db_open(const char *cache_dir) {
  if (!make_dir_recursive(cache_dir))
    return NULL;

  char packages_path[4096];
  snprintf(packages_path, sizeof(packages_path), "%s/cache", cache_dir);
  if (!make_dir_recursive(packages_path))
    return NULL;

  MDB_env *env = NULL;
  if (mdb_env_create(&env) != 0)
    return NULL;

  if (mdb_env_set_mapsize(env, MAP_SIZE) != 0 ||
      mdb_env_set_maxdbs(env, 3) != 0) {
    mdb_env_close(env);
    return NULL;
  }

  char db_path[4096];
  snprintf(db_path, sizeof(db_path), "%s/index.lmdb", cache_dir);
  if (mdb_env_open(env, db_path, MDB_NOSUBDIR | MDB_NOSYNC, 0644) != 0) {
    mdb_env_close(env);
    return NULL;
  }

  cache_db_t *db = malloc(sizeof(cache_db_t));
  if (!db) {
    mdb_env_close(env);
    return NULL;
  }

  db->env = env;
  db->cache_dir = strdup(cache_dir);

  MDB_txn *txn = NULL;
  if (mdb_txn_begin(env, NULL, 0, &txn) != 0) {
    free(db->cache_dir);
    free(db);
    mdb_env_close(env);
    return NULL;
  }

  if (mdb_dbi_open(txn, "primary", MDB_CREATE, &db->dbi_primary) != 0 ||
      mdb_dbi_open(txn, "secondary", MDB_CREATE, &db->dbi_secondary) != 0 ||
      mdb_dbi_open(txn, "metadata", MDB_CREATE, &db->dbi_metadata) != 0) {
    mdb_txn_abort(txn);
    free(db->cache_dir);
    free(db);
    mdb_env_close(env);
    return NULL;
  }

  if (mdb_txn_commit(txn) != 0) {
    free(db->cache_dir);
    free(db);
    mdb_env_close(env);
    return NULL;
  }

  // Auto prune metadata on open
  if (mdb_txn_begin(env, NULL, 0, &txn) == 0) {
    prune_expired_metadata_txn(db, txn);
    mdb_txn_commit(txn);
  }

  return db;
}

void cache_db_close(cache_db_t *db) {
  mdb_dbi_close(db->env, db->dbi_primary);
  mdb_dbi_close(db->env, db->dbi_secondary);
  mdb_dbi_close(db->env, db->dbi_metadata);
  mdb_env_close(db->env);
  free(db->cache_dir);
  free(db);
}

static bool deserialize_entry(const uint8_t integrity[64], MDB_val val,
                              cache_entry_t *out_entry) {
  if (val.mv_size < sizeof(cache_serialized_entry_t))
    return false;

  const cache_serialized_entry_t *header =
      (const cache_serialized_entry_t *)val.mv_data;
  size_t path_start = sizeof(cache_serialized_entry_t);
  if (val.mv_size < path_start + header->path_len)
    return false;

  memcpy(out_entry->integrity, integrity, 64);
  out_entry->unpacked_size = header->unpacked_size;
  out_entry->file_count = header->file_count;
  out_entry->cached_at = header->cached_at;

  out_entry->path = malloc(header->path_len + 1);
  if (!out_entry->path)
    return false;

  memcpy(out_entry->path, (const char *)val.mv_data + path_start,
         header->path_len);
  out_entry->path[header->path_len] = '\0';

  return true;
}

bool cache_db_lookup(cache_db_t *db, const uint8_t integrity[64],
                     cache_entry_t *out_entry) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;

  uint8_t key_bytes[66];
  make_integrity_key(integrity, key_bytes);

  MDB_val key = {.mv_size = 66, .mv_data = key_bytes};
  MDB_val val;

  if (mdb_get(txn, db->dbi_primary, &key, &val) != 0) {
    mdb_txn_abort(txn);
    return false;
  }

  bool success = deserialize_entry(integrity, val, out_entry);
  mdb_txn_abort(txn);
  return success;
}

bool cache_db_has_integrity(cache_db_t *db, const uint8_t integrity[64]) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;

  uint8_t key_bytes[66];
  make_integrity_key(integrity, key_bytes);

  MDB_val key = {.mv_size = 66, .mv_data = key_bytes};
  MDB_val val;

  bool found = (mdb_get(txn, db->dbi_primary, &key, &val) == 0);
  mdb_txn_abort(txn);
  return found;
}

bool cache_db_lookup_by_name(cache_db_t *db, const char *name,
                             const char *version, cache_entry_t *out_entry) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;

  char *name_key = make_name_key(name, version);
  if (!name_key) {
    mdb_txn_abort(txn);
    return false;
  }

  MDB_val key = {.mv_size = strlen(name_key), .mv_data = name_key};
  MDB_val val;

  if (mdb_get(txn, db->dbi_secondary, &key, &val) != 0 || val.mv_size != 64) {
    free(name_key);
    mdb_txn_abort(txn);
    return false;
  }

  free(name_key);

  const uint8_t *integrity = (const uint8_t *)val.mv_data;
  uint8_t key_bytes[66];
  make_integrity_key(integrity, key_bytes);

  key.mv_size = 66;
  key.mv_data = key_bytes;

  if (mdb_get(txn, db->dbi_primary, &key, &val) != 0) {
    mdb_txn_abort(txn);
    return false;
  }

  bool success = deserialize_entry(integrity, val, out_entry);
  mdb_txn_abort(txn);
  return success;
}

size_t cache_db_batch_lookup(cache_db_t *db, const uint8_t (*integrities)[64],
                             size_t count, cache_batch_hit_t *out_hits) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) != 0)
    return 0;

  size_t hit_count = 0;

  for (size_t i = 0; i < count; i++) {
    uint8_t key_bytes[66];
    make_integrity_key(integrities[i], key_bytes);

    MDB_val key = {.mv_size = 66, .mv_data = key_bytes};
    MDB_val val;

    if (mdb_get(txn, db->dbi_primary, &key, &val) == 0) {
      uint32_t file_count = 0;
      if (val.mv_size >= sizeof(cache_serialized_entry_t)) {
        const cache_serialized_entry_t *header =
            (const cache_serialized_entry_t *)val.mv_data;
        file_count = header->file_count;
      }
      out_hits[hit_count++] =
          (cache_batch_hit_t){.index = (uint32_t)i, .file_count = file_count};
    }
  }

  mdb_txn_abort(txn);
  return hit_count;
}

static bool cache_db_insert_in_txn(cache_db_t *db, MDB_txn *txn,
                                   const cache_entry_t *entry, const char *name,
                                   const char *version) {
  size_t path_len = strlen(entry->path);
  size_t val_size = sizeof(cache_serialized_entry_t) + path_len;
  uint8_t *val_buf = malloc(val_size);
  if (!val_buf)
    return false;

  cache_serialized_entry_t *header = (cache_serialized_entry_t *)val_buf;
  header->unpacked_size = entry->unpacked_size;
  header->file_count = entry->file_count;
  header->cached_at = entry->cached_at;
  header->path_len = (uint32_t)path_len;

  memcpy(val_buf + sizeof(cache_serialized_entry_t), entry->path, path_len);

  uint8_t key_bytes[66];
  make_integrity_key(entry->integrity, key_bytes);

  MDB_val key = {.mv_size = 66, .mv_data = key_bytes};
  MDB_val val = {.mv_size = val_size, .mv_data = val_buf};

  if (mdb_put(txn, db->dbi_primary, &key, &val, 0) != 0) {
    free(val_buf);
    return false;
  }
  free(val_buf);

  if (name && version) {
    char *name_key = make_name_key(name, version);
    if (!name_key)
      return false;

    MDB_val sec_key = {.mv_size = strlen(name_key), .mv_data = name_key};
    MDB_val sec_val = {.mv_size = 64, .mv_data = (void *)entry->integrity};

    if (mdb_put(txn, db->dbi_secondary, &sec_key, &sec_val, 0) != 0) {
      free(name_key);
      return false;
    }
    free(name_key);
  }

  return true;
}

bool cache_db_insert(cache_db_t *db, const cache_entry_t *entry,
                     const char *name, const char *version) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return false;

  if (!cache_db_insert_in_txn(db, txn, entry, name, version)) {
    mdb_txn_abort(txn);
    return false;
  }

  return mdb_txn_commit(txn) == 0;
}

bool cache_db_batch_insert(cache_db_t *db, const cache_entry_t *entries,
                           size_t count) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return false;

  for (size_t i = 0; i < count; i++) {
    if (!cache_db_insert_in_txn(db, txn, &entries[i], NULL, NULL)) {
      mdb_txn_abort(txn);
      return false;
    }
  }

  return mdb_txn_commit(txn) == 0;
}

bool cache_db_batch_insert_named(cache_db_t *db,
                                 const cache_named_entry_t *entries,
                                 size_t count) {
  if (count == 0)
    return true;

  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return false;

  for (size_t i = 0; i < count; i++) {
    if (!cache_db_insert_in_txn(db, txn, &entries[i].entry, entries[i].name,
                                entries[i].version)) {
      // Continue inserting others as in Zig loop
      continue;
    }
  }

  return mdb_txn_commit(txn) == 0;
}

bool cache_db_delete(cache_db_t *db, const uint8_t integrity[64]) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return false;

  uint8_t key_bytes[66];
  make_integrity_key(integrity, key_bytes);

  MDB_val key = {.mv_size = 66, .mv_data = key_bytes};

  mdb_del(txn, db->dbi_primary, &key, NULL);
  return mdb_txn_commit(txn) == 0;
}

char *cache_db_get_package_path(cache_db_t *db, const uint8_t integrity[64]) {
  char hex[129];
  bytes_to_hex(integrity, 64, hex);
  size_t len = strlen(db->cache_dir) + strlen(hex) + 8;
  char *path = malloc(len);
  if (!path)
    return NULL;
  snprintf(path, len, "%s/cache/%s", db->cache_dir, hex);
  return path;
}

void cache_db_sync(cache_db_t *db) { mdb_env_sync(db->env, 1); }

bool cache_db_stats(cache_db_t *db, size_t *out_entries, size_t *out_db_size,
                    size_t *out_cache_size) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) != 0)
    return false;

  MDB_stat db_stat;
  mdb_stat(txn, db->dbi_primary, &db_stat);
  mdb_txn_abort(txn);

  *out_entries = db_stat.ms_entries;

  char db_path[4096];
  snprintf(db_path, sizeof(db_path), "%s/index.lmdb", db->cache_dir);
  struct stat st;
  if (stat(db_path, &st) == 0) {
    *out_db_size = align_to_block(st.st_size);
  } else {
    *out_db_size = 0;
  }

  char cache_path[4096];
  snprintf(cache_path, sizeof(cache_path), "%s/cache", db->cache_dir);
  *out_cache_size = get_dir_size_recursive(cache_path);

  return true;
}

char *cache_db_lookup_metadata(cache_db_t *db, const char *name) {
  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn) != 0)
    return NULL;

  char *meta_key = make_metadata_key(name);
  if (!meta_key) {
    mdb_txn_abort(txn);
    return NULL;
  }

  MDB_val key = {.mv_size = strlen(meta_key), .mv_data = meta_key};
  MDB_val val;

  if (mdb_get(txn, db->dbi_metadata, &key, &val) != 0 ||
      val.mv_size < sizeof(int64_t)) {
    free(meta_key);
    mdb_txn_abort(txn);
    return NULL;
  }

  free(meta_key);

  int64_t cached_at;
  memcpy(&cached_at, val.mv_data, sizeof(int64_t));

  int64_t now = (int64_t)time(NULL);
  if (now - cached_at > METADATA_TTL_SECS) {
    mdb_txn_abort(txn);
    return NULL;
  }

  size_t json_len = val.mv_size - sizeof(int64_t);
  char *json_copy = malloc(json_len + 1);
  if (!json_copy) {
    mdb_txn_abort(txn);
    return NULL;
  }

  memcpy(json_copy, (const char *)val.mv_data + sizeof(int64_t), json_len);
  json_copy[json_len] = '\0';

  mdb_txn_abort(txn);
  return json_copy;
}

bool cache_db_insert_metadata(cache_db_t *db, const char *name,
                              const char *json_data, size_t json_len) {
  size_t stripped_len = 0;
  char *stripped_ptr = strip_npm_metadata(json_data, json_len, &stripped_len);

  const char *data_to_store = stripped_ptr ? stripped_ptr : json_data;
  size_t len_to_store = stripped_ptr ? stripped_len : json_len;

  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0) {
    if (stripped_ptr)
      strip_metadata_free(stripped_ptr);
    return false;
  }

  char *meta_key = make_metadata_key(name);
  if (!meta_key) {
    if (stripped_ptr)
      strip_metadata_free(stripped_ptr);
    mdb_txn_abort(txn);
    return false;
  }

  size_t val_size = sizeof(int64_t) + len_to_store;
  uint8_t *val_buf = malloc(val_size);
  if (!val_buf) {
    free(meta_key);
    if (stripped_ptr)
      strip_metadata_free(stripped_ptr);
    mdb_txn_abort(txn);
    return false;
  }

  int64_t now = (int64_t)time(NULL);
  memcpy(val_buf, &now, sizeof(int64_t));
  memcpy(val_buf + sizeof(int64_t), data_to_store, len_to_store);

  MDB_val key = {.mv_size = strlen(meta_key), .mv_data = meta_key};
  MDB_val val = {.mv_size = val_size, .mv_data = val_buf};

  if (mdb_put(txn, db->dbi_metadata, &key, &val, 0) != 0) {
    free(val_buf);
    free(meta_key);
    if (stripped_ptr)
      strip_metadata_free(stripped_ptr);
    mdb_txn_abort(txn);
    return false;
  }

  free(val_buf);
  free(meta_key);
  if (stripped_ptr)
    strip_metadata_free(stripped_ptr);

  return mdb_txn_commit(txn) == 0;
}

int32_t cache_db_prune(cache_db_t *db, uint32_t max_age_days) {
  int64_t now = (int64_t)time(NULL);
  int64_t cutoff = now - (int64_t)max_age_days * 24 * 60 * 60;

  MDB_txn *txn = NULL;
  if (mdb_txn_begin(db->env, NULL, 0, &txn) != 0)
    return -1;

  MDB_cursor *cursor = NULL;
  if (mdb_cursor_open(txn, db->dbi_primary, &cursor) != 0) {
    mdb_txn_abort(txn);
    return -1;
  }

  size_t key_cap = 16;
  uint8_t (*keys_to_delete)[66] = malloc(key_cap * sizeof(*keys_to_delete));
  char **paths_to_delete = malloc(key_cap * sizeof(char *));
  size_t delete_count = 0;

  MDB_val key, val;
  int rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST);

  while (rc == 0) {
    if (val.mv_size >= sizeof(cache_serialized_entry_t)) {
      const cache_serialized_entry_t *header =
          (const cache_serialized_entry_t *)val.mv_data;
      if (header->cached_at < cutoff && key.mv_size == 66) {
        if (delete_count >= key_cap) {
          key_cap *= 2;
          keys_to_delete =
              realloc(keys_to_delete, key_cap * sizeof(*keys_to_delete));
          paths_to_delete = realloc(paths_to_delete, key_cap * sizeof(char *));
        }
        memcpy(keys_to_delete[delete_count], key.mv_data, 66);

        size_t path_start = sizeof(cache_serialized_entry_t);
        char *path = malloc(header->path_len + 1);
        if (path) {
          memcpy(path, (const char *)val.mv_data + path_start,
                 header->path_len);
          path[header->path_len] = '\0';
          paths_to_delete[delete_count] = path;
        } else {
          paths_to_delete[delete_count] = NULL;
        }
        delete_count++;
      }
    }
    rc = mdb_cursor_get(cursor, &key, &val, MDB_NEXT);
  }
  mdb_cursor_close(cursor);

  // Perform deletions
  uint32_t pruned = 0;
  for (size_t i = 0; i < delete_count; i++) {
    MDB_val del_key = {.mv_size = 66, .mv_data = keys_to_delete[i]};
    if (mdb_del(txn, db->dbi_primary, &del_key, NULL) == 0) {
      pruned++;
    }
  }

  // Prune metadata in metadata DB
  prune_expired_metadata_txn(db, txn);

  // Prune secondary DB stale index entries
  MDB_cursor *sec_cursor = NULL;
  if (mdb_cursor_open(txn, db->dbi_secondary, &sec_cursor) == 0) {
    rc = mdb_cursor_get(sec_cursor, &key, &val, MDB_FIRST);
    while (rc == 0) {
      if (val.mv_size == 64) {
        uint8_t chk_bytes[66];
        make_integrity_key((const uint8_t *)val.mv_data, chk_bytes);
        MDB_val chk_key = {.mv_size = 66, .mv_data = chk_bytes};
        MDB_val chk_val;
        if (mdb_get(txn, db->dbi_primary, &chk_key, &chk_val) != 0) {
          // Primary entry no longer exists, delete secondary index
          mdb_cursor_del(sec_cursor, 0);
        }
      }
      rc = mdb_cursor_get(sec_cursor, &key, &val, MDB_NEXT);
    }
    mdb_cursor_close(sec_cursor);
  }

  if (mdb_txn_commit(txn) != 0) {
    for (size_t i = 0; i < delete_count; i++) {
      if (paths_to_delete[i])
        free(paths_to_delete[i]);
    }
    free(keys_to_delete);
    free(paths_to_delete);
    return -1;
  }

  for (size_t i = 0; i < delete_count; i++) {
    if (paths_to_delete[i]) {
      cache_delete_tree(paths_to_delete[i]);
      free(paths_to_delete[i]);
    }
  }

  free(keys_to_delete);
  free(paths_to_delete);

  return (int32_t)pruned;
}
