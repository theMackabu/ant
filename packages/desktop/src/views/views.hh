#pragma once

#include "desktop.hh"
#include "views/system.hh"

namespace desktop::views {

std::string ApplicationInfo(const std::string &key);
std::string ApplicationName();
std::string ApplicationDirectory();
void SetMenu(const char *json);

}
