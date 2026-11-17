#include "desktop.hh"

#include <unistd.h>

#include "include/wrapper/cef_library_loader.h"

namespace desktop::platform {

static constexpr char kFrameworkVariable[] = "ANT_DESKTOP_CEF_FRAMEWORK";
static constexpr char kFrameworkSwitch[] = "--framework-dir-path=";
static constexpr char kHelper[] = "/Contents/Frameworks/Ant Desktop Helper.app/Contents/MacOS/Ant Desktop Helper";

static bool LoadFramework(const std::string &framework) {
  return cef_load_library((framework + "/Chromium Embedded Framework").c_str());
}

bool ConfigureBrowser(CefSettings &settings) {
  std::string framework =
    Owned(AntDesktop_CEF_Resolve(ANT_DESKTOP_CEF_VERSION, ANT_DESKTOP_CEF_ARCHIVE, ANT_DESKTOP_CEF_SHA1, ANT_DESKTOP_CEF_SIZE));
  if (framework.empty() || !LoadFramework(framework)) return false;
  setenv(kFrameworkVariable, framework.c_str(), 1);

  std::string bundle = Owned(AntDesktop_App_Path("bundle"));
  std::string helper = bundle + kHelper;
  
  if (access(helper.c_str(), X_OK) != 0) {
    fprintf(stderr, "missing helper application: %s\n", helper.c_str());
    return false;
  }

  CefString(&settings.browser_subprocess_path) = helper;
  CefString(&settings.framework_dir_path) = framework;
  CefString(&settings.main_bundle_path) = bundle;
  CefString(&settings.resources_dir_path) = framework + "/Resources";
  CefString(&settings.locales_dir_path) = framework + "/Resources/locales";
  
  return true;
}

bool LoadHelperBrowser(int argc, char **argv) {
  std::string framework = getenv(kFrameworkVariable) ?: "";
  for (int index = 1; index < argc; index++) {
    std::string argument = argv[index];
    if (argument.starts_with(kFrameworkSwitch)) framework = argument.substr(sizeof(kFrameworkSwitch) - 1);
  }
  return LoadFramework(framework);
}

void ReleaseBrowser() {
  cef_unload_library();
}

}
