#pragma once

#include <stddef.h>
#include <stdbool.h>

int ant_sandbox_assets_download_missing(
  const char *image_path,
  const char *kernel_path,
  char *err,
  size_t err_len
);
