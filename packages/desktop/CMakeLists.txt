cmake_minimum_required(VERSION 3.24)

set(DESKTOP_MACOS_TARGET "15.0")
set(CMAKE_BUILD_TYPE Release CACHE STRING "")
project(ant_desktop C CXX)

foreach(name CEF_ROOT LIBANT_ROOT DESKTOP_VERSION CEF_VERSION CEF_ARCHIVE CEF_SHA1 CEF_SIZE CHROMIUM_VERSION)
  if(NOT ${name})
    message(FATAL_ERROR "Configure through scripts/build.js (missing -D${name})")
  endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
list(REMOVE_ITEM CEF_COMPILER_FLAGS -Werror)
if(APPLE)
  list(REMOVE_ITEM CEF_COMPILER_FLAGS "-mmacosx-version-min=${CEF_TARGET_SDK}")
  list(APPEND CEF_COMPILER_FLAGS "-mmacosx-version-min=${DESKTOP_MACOS_TARGET}")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${DESKTOP_MACOS_TARGET}")
endif()
add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

set(ENV{PKG_CONFIG_PATH} "${LIBANT_ROOT}/lib/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBANT REQUIRED libant)

set(RUNTIME_DEFINITIONS
  ANT_DESKTOP_CEF_VERSION="${CEF_VERSION}"
  ANT_DESKTOP_CEF_ARCHIVE="${CEF_ARCHIVE}"
  ANT_DESKTOP_CEF_SHA1="${CEF_SHA1}"
  ANT_DESKTOP_CEF_SIZE=${CEF_SIZE}
)
if(MSVC)
  set(DESKTOP_OPTIONS /GR- -Wno-c23-extensions ${CEF_RUNTIME_LIBRARY_FLAG})
else()
  set(DESKTOP_OPTIONS -fno-rtti -fvisibility=hidden -Wall -Wno-unused-parameter -Wno-c23-extensions)
endif()

file(GLOB_RECURSE BUILTIN_SOURCES CONFIGURE_DEPENDS src/builtins/*.js)
add_library(antdesktop SHARED
  src/main.cc
  src/ant/integration.cc
  src/ant/module.cc
  src/ant/runtime.cc
  src/cef/browser.cc
  src/cef/renderer.cc
  src/cef/scheme.cc
)
set_source_files_properties(src/ant/module.cc PROPERTIES OBJECT_DEPENDS "${BUILTIN_SOURCES}")
target_compile_features(antdesktop PRIVATE cxx_std_23)
target_compile_options(antdesktop PRIVATE ${DESKTOP_OPTIONS} ${LIBANT_CFLAGS})
target_compile_definitions(antdesktop PRIVATE
  ANT_DESKTOP_VERSION="${DESKTOP_VERSION}"
  ANT_DESKTOP_CHROMIUM_VERSION="${CHROMIUM_VERSION}"
  ${RUNTIME_DEFINITIONS}
)
target_include_directories(antdesktop PRIVATE "${CEF_ROOT}" src)
target_link_libraries(antdesktop PRIVATE libcef_dll_wrapper ${LIBANT_LDFLAGS})

if(APPLE)
  set(SWIFT_TARGET "${CMAKE_SYSTEM_PROCESSOR}-apple-macos${DESKTOP_MACOS_TARGET}")
  set(SWIFT_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/desktop_swift.o")
  file(GLOB SWIFT_SOURCES CONFIGURE_DEPENDS src/darwin/*.swift)
  execute_process(COMMAND xcrun --find swiftc OUTPUT_VARIABLE SWIFTC OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND xcrun --sdk macosx --show-sdk-path OUTPUT_VARIABLE MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
  cmake_path(GET SWIFTC PARENT_PATH SWIFT_TOOLCHAIN_BIN)

  add_custom_command(
    OUTPUT "${SWIFT_OBJECT}"
    COMMAND "${SWIFTC}" -O -wmo -parse-as-library -swift-version 5 -target "${SWIFT_TARGET}" -sdk "${MACOS_SDK}"
      -module-name AntDesktopUI -import-objc-header "${CMAKE_CURRENT_SOURCE_DIR}/src/darwin/bridging.hh"
      -emit-object -o "${SWIFT_OBJECT}" ${SWIFT_SOURCES}
    DEPENDS ${SWIFT_SOURCES} src/darwin/bridging.hh src/platform.hh
    VERBATIM
  )

  target_sources(antdesktop PRIVATE src/darwin/cef.cc src/darwin/loop.cc "${SWIFT_OBJECT}")
  target_link_directories(antdesktop PRIVATE "${SWIFT_TOOLCHAIN_BIN}/../lib/swift/macosx" "${MACOS_SDK}/usr/lib/swift")
  target_link_libraries(antdesktop PRIVATE "-framework AppKit" "-framework CryptoKit" "-framework QuartzCore")
  target_link_options(antdesktop PRIVATE -Wl,-dead_strip -Wl,-rpath,/usr/lib/swift)
  set_target_properties(antdesktop PROPERTIES INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR ON)

  add_executable(ant-desktop src/darwin/launcher.cc)
  target_link_libraries(ant-desktop PRIVATE antdesktop)
  set_target_properties(ant-desktop PROPERTIES
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@executable_path/../Frameworks;@executable_path/../../.."
  )
else()
  if(WIN32)
    set(DESKTOP_SYSTEM windows)
  else()
    set(DESKTOP_SYSTEM linux)
  endif()
  ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

  target_sources(antdesktop PRIVATE
    src/views/app.cc
    src/views/window.cc
    src/${DESKTOP_SYSTEM}/cef.cc
    src/${DESKTOP_SYSTEM}/system.cc
  )
  target_link_libraries(antdesktop PRIVATE libcef_lib ${CEF_STANDARD_LIBS})

  add_executable(ant-desktop src/views/launcher.cc src/${DESKTOP_SYSTEM}/system.cc)
  target_compile_features(ant-desktop PRIVATE cxx_std_23)
  target_compile_options(ant-desktop PRIVATE ${DESKTOP_OPTIONS})
  target_compile_definitions(ant-desktop PRIVATE ${RUNTIME_DEFINITIONS})
  target_include_directories(ant-desktop PRIVATE src)
  target_link_libraries(ant-desktop PRIVATE ${CMAKE_DL_LIBS})
  if(WIN32)
    target_sources(ant-desktop PRIVATE src/windows/app.manifest)
    target_link_libraries(antdesktop PRIVATE shell32 ole32 user32)
    target_link_libraries(ant-desktop PRIVATE shell32 ole32 user32)
    target_link_options(ant-desktop PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
  endif()
endif()
