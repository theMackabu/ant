#include "views/system.hh"

#include <dlfcn.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <thread>

extern char **environ;

namespace desktop::system {

const char kBrowserLibrary[] = "libcef.so";
const char kDesktopLibrary[] = "libantdesktop.so";

static int reload_pipe[2];

static std::string Environment(const char *name, const std::string &fallback) {
  const char *value = getenv(name);
  return value && *value ? value : fallback;
}

static std::string Home() {
  return Environment("HOME", "/");
}

std::string ExecutablePath() {
  char buffer[PATH_MAX];
  ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer));
  return length > 0 ? std::string(buffer, size_t(length)) : "";
}

std::string Path(const std::string &name) {
  if (name == "home") return Home();
  if (name == "temp") return Environment("TMPDIR", "/tmp");
  if (name == "appData") return Environment("XDG_CONFIG_HOME", Home() + "/.config");
  if (name == "desktop") return Home() + "/Desktop";
  if (name == "documents") return Home() + "/Documents";
  if (name == "downloads") return Home() + "/Downloads";
  return "";
}

std::string SharedRuntimeDirectory() {
  return Environment("XDG_DATA_HOME", Home() + "/.local/share") + "/ant-desktop/cef";
}

void SetEnvironment(const char *name, const std::string &value) {
  setenv(name, value.c_str(), 1);
}

void *OpenLibrary(const std::string &path) {
  return dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
}

void *FindSymbol(void *library, const char *name) {
  return dlsym(library, name);
}

int Run(const std::vector<std::string> &command, bool visible) {
  std::vector<char *> argv;
  for (const std::string &argument : command) argv.push_back(const_cast<char *>(argument.c_str()));
  argv.push_back(nullptr);
  pid_t child;
  if (posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
  int status;
  while (waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void WaitBackend(intptr_t backend) {
  pollfd descriptor = {int(backend), POLLIN, 0};
  while (poll(&descriptor, 1, -1) < 0 && errno == EINTR) {}
}

void OnReloadSignal(void (*reload)()) {
  if (pipe(reload_pipe) != 0) return;
  signal(SIGUSR1, [](int) {
    char byte = 0;
    (void)!write(reload_pipe[1], &byte, 1);
  });
  std::thread([reload] {
    char byte;
    for (;;) {
      ssize_t count = read(reload_pipe[0], &byte, 1);
      if (count == 1) reload();
      else if (count == 0 || errno != EINTR) return;
    }
  }).detach();
}

bool SignalReload(int64_t process) {
  return kill(pid_t(process), SIGUSR1) == 0;
}

void StyleWindow(void *handle, double opacity, bool protect) {}

}
