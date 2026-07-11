cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0156)
  cmake_policy(SET CMP0156 NEW)
endif()

set(CMAKE_CONFIGURATION_TYPES Debug Release)
project(ant_desktop_runtime)

if(NOT CEF_ROOT)
  message(FATAL_ERROR "Pass -DCEF_ROOT=/path/to/cef_binary_*")
endif()
if(NOT LIBANT_ROOT OR NOT EXISTS "${LIBANT_ROOT}/libant.a")
  message(FATAL_ERROR "Pass -DLIBANT_ROOT=/path/to/libant/dist")
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
list(REMOVE_ITEM CEF_COMPILER_FLAGS "-mmacosx-version-min=${CEF_TARGET_SDK}")
list(APPEND CEF_COMPILER_FLAGS "-mmacosx-version-min=15.0")
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

set(IPC_BRIDGE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/../../ipc/bridge/clone.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../ipc/bridge/renderer.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../ipc/bridge/main.js"
)
find_program(NODE_EXECUTABLE node REQUIRED)
set(IPC_BRIDGE_EMBEDDER "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/embed-renderer-bridge.cjs")
set(IPC_BRIDGE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/renderer_bridge.h")
add_custom_command(
  OUTPUT "${IPC_BRIDGE_HEADER}"
  COMMAND "${NODE_EXECUTABLE}" "${IPC_BRIDGE_EMBEDDER}" "${IPC_BRIDGE_HEADER}"
  DEPENDS ${IPC_BRIDGE_SOURCES} "${IPC_BRIDGE_EMBEDDER}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../package.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../packaging/npm/darwin-arm64/runtime.lock.json"
  VERBATIM
)
add_custom_target(ant_desktop_ipc_bridge DEPENDS "${IPC_BRIDGE_HEADER}")

set(CEF_TARGET ant_chromium_runtime_stage)
set(CEF_OUTPUT_NAME "Ant Desktop")
set(CEF_HELPER_TARGET ant_desktop_helper)
set(CEF_HELPER_OUTPUT_NAME "${CEF_OUTPUT_NAME} Helper")

SET_CEF_TARGET_OUT_DIR()
set(CEF_APP "${CEF_TARGET_OUT_DIR}/${CEF_OUTPUT_NAME}.app")
set(EXECUTABLE_NAME "${CEF_OUTPUT_NAME}")
set(PRODUCT_NAME "${CEF_OUTPUT_NAME}")

add_executable(${CEF_TARGET} MACOSX_BUNDLE
  runtime_stage.mm
)
SET_EXECUTABLE_TARGET_PROPERTIES(${CEF_TARGET})
target_compile_options(${CEF_TARGET} PRIVATE -fobjc-arc)
target_link_libraries(${CEF_TARGET}
  libcef_dll_wrapper
  ${CEF_STANDARD_LIBS}
)
set_target_properties(${CEF_TARGET} PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/../mac/Info.plist.in"
  OUTPUT_NAME "${CEF_OUTPUT_NAME}"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.antjs.desktop-runtime-stage"
)

COPY_MAC_FRAMEWORK("${CEF_TARGET}" "${CEF_BINARY_DIR}" "${CEF_APP}")

foreach(_suffix_list ${CEF_HELPER_APP_SUFFIXES})
  string(REPLACE ":" ";" _suffix_list ${_suffix_list})
  list(GET _suffix_list 0 _name_suffix)
  list(GET _suffix_list 1 _target_suffix)
  list(GET _suffix_list 2 _plist_suffix)

  set(_helper_target "${CEF_HELPER_TARGET}${_target_suffix}")
  set(_helper_output_name "${CEF_HELPER_OUTPUT_NAME}${_name_suffix}")
  set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")

  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../mac/helper-Info.plist.in" _plist_contents)
  string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents "${_plist_contents}")
  string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents "${_plist_contents}")
  string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents "${_plist_contents}")
  string(REPLACE "\${VERSION_SHORT}" "${VERSION_SHORT}" _plist_contents "${_plist_contents}")
  file(WRITE "${_helper_info_plist}" "${_plist_contents}")

  set(_helper_sources
    process_helper.cc
    renderer_app.cc
    ../../ipc/capabilities.cc
  )
  if(_target_suffix STREQUAL "_renderer")
    list(APPEND _helper_sources
      ant_module_bindings.cc
      node_bindings.cc
      renderer_ant_runtime.c
      ../../app/runtime/ant_runtime.c
    )
  endif()
  add_executable(${_helper_target} MACOSX_BUNDLE ${_helper_sources})
  SET_EXECUTABLE_TARGET_PROPERTIES(${_helper_target})
  target_compile_options(${_helper_target} PRIVATE -Wno-typedef-redefinition)
  add_dependencies(${_helper_target} libcef_dll_wrapper)
  add_dependencies(${_helper_target} ant_desktop_ipc_bridge)
  target_include_directories(${_helper_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  target_link_libraries(${_helper_target}
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS})
  if(_target_suffix STREQUAL "_renderer")
    target_compile_definitions(${_helper_target} PRIVATE ANT_DESKTOP_RUNTIME_INTEGRATION=1)
    target_include_directories(${_helper_target} PRIVATE "${LIBANT_ROOT}")
    target_link_libraries(${_helper_target}
      "${LIBANT_ROOT}/libant.a"
      "-framework Security"
      "-framework CoreFoundation"
      "-framework Hypervisor"
      "-lpthread")
  endif()
  set_target_properties(${_helper_target} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${_helper_info_plist}"
    OUTPUT_NAME "${_helper_output_name}"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "org.antjs.desktop.helper${_plist_suffix}"
  )

  add_dependencies(${CEF_TARGET} ${_helper_target})
  add_custom_command(
    TARGET ${CEF_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${CEF_TARGET_OUT_DIR}/${_helper_output_name}.app"
      "${CEF_APP}/Contents/Frameworks/${_helper_output_name}.app"
    VERBATIM
  )
endforeach()

PRINT_CEF_CONFIG()
