#ifndef ANT_ZIP_H
#define ANT_ZIP_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

// extracts the only entry of a zip archive, which must be named `name`;
// supports stored and deflated entries without zip64 or encryption
int ant_zip_extract_single(const uint8_t *zip, size_t zip_len, const char *name, FILE *out, char *err, size_t err_len);

#endif
