add_library(libuv::uv_a INTERFACE IMPORTED)
set_target_properties(libuv::uv_a PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../include"
)
