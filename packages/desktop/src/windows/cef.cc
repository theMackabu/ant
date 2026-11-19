#include "desktop.hh"

#include <windows.h>

namespace desktop::platform {

CefMainArgs MainArgs(int argc, char **argv) {
  return CefMainArgs(GetModuleHandleW(nullptr));
}

}
