add_library(llhttp::llhttp_static INTERFACE IMPORTED)
set_target_properties(llhttp::llhttp_static PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/../include"
)
add_library(llhttp::llhttp ALIAS llhttp::llhttp_static)
