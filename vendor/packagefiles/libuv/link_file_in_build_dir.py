#!/usr/bin/env python3

import os, sys

filename = sys.argv[1]
linkname = sys.argv[2]

build_dir = os.path.join(
    os.getenv('MESON_BUILD_ROOT'),
    os.getenv('MESON_SUBDIR')
)

src = os.path.join(build_dir, filename)
dst = os.path.join(build_dir, linkname)

os.symlink(src, dst)
