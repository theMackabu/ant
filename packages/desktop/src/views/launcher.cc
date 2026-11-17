#include "views/system.hh"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace desktop;

static constexpr char kDownloadHost[] = "https://cef-builds.spotifycdn.com/";

static uint32_t Rotate(uint32_t value, int bits) {
  return value << bits | value >> (32 - bits);
}

static void HashBlock(std::array<uint32_t, 5> &state, const uint8_t *block) {
  uint32_t words[80];
  for (int index = 0; index < 16; index++) {
    words[index] = uint32_t(block[index * 4]) << 24 | uint32_t(block[index * 4 + 1]) << 16 |
                   uint32_t(block[index * 4 + 2]) << 8 | uint32_t(block[index * 4 + 3]);
  }
  for (int index = 16; index < 80; index++) {
    words[index] = Rotate(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
  for (int index = 0; index < 80; index++) {
    uint32_t mix = index < 20   ? ((b & c) | (~b & d)) + 0x5a827999
                   : index < 40 ? (b ^ c ^ d) + 0x6ed9eba1
                   : index < 60 ? ((b & c) | (b & d) | (c & d)) + 0x8f1bbcdc
                                : (b ^ c ^ d) + 0xca62c1d6;
    uint32_t next = Rotate(a, 5) + mix + e + words[index];
    e = d;
    d = c;
    c = Rotate(b, 30);
    b = a;
    a = next;
  }
  state[0] += a, state[1] += b, state[2] += c, state[3] += d, state[4] += e;
}

static std::string HashFile(const fs::path &file, uint64_t *size) {
  std::array<uint32_t, 5> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  std::ifstream input(file, std::ios::binary);
  uint8_t block[128] = {};
  uint64_t total = 0;
  size_t filled = 0;
  while (input.read(reinterpret_cast<char *>(block), 64) || input.gcount() > 0) {
    filled = size_t(input.gcount());
    total += filled;
    if (filled < 64) break;
    HashBlock(state, block);
    filled = 0;
  }
  memset(block + filled, 0, sizeof(block) - filled);
  block[filled] = 0x80;
  size_t padded = filled < 56 ? 64 : 128;
  for (int index = 0; index < 8; index++) block[padded - 1 - index] = uint8_t(total * 8 >> (index * 8));
  for (size_t offset = 0; offset < padded; offset += 64) HashBlock(state, block + offset);

  char digest[41];
  snprintf(digest, sizeof(digest), "%08x%08x%08x%08x%08x", state[0], state[1], state[2], state[3], state[4]);
  *size = total;
  return digest;
}

static bool Installed(const fs::path &directory) {
  std::error_code error;
  return !directory.empty() && fs::exists(directory / system::kBrowserLibrary, error);
}

static bool Download(const fs::path &destination) {
  std::string archive = ANT_DESKTOP_CEF_ARCHIVE;
  std::string root = archive.substr(0, archive.size() - strlen(".tar.bz2"));
  std::string url = kDownloadHost;
  for (char unit : archive) url += unit == '+' ? "%2B" : std::string(1, unit);

  std::error_code error;
  fs::path staging = destination.parent_path() / ("." + destination.filename().string() + ".staging");
  fs::path file = destination.parent_path() / ("." + destination.filename().string() + ".download");
  fs::remove_all(staging, error);
  fs::create_directories(staging, error);

  fprintf(stderr, "Downloading the browser runtime (%d MB). This happens once.\n", int(ANT_DESKTOP_CEF_SIZE / 1000000));
  bool ready = system::Run({"curl", "-fL", "--retry", "3", "-o", file.string(), url}, true) == 0;
  uint64_t size = 0;
  if (!ready) fprintf(stderr, "The browser runtime download failed.\n");
  else if (HashFile(file, &size) != ANT_DESKTOP_CEF_SHA1 || size != ANT_DESKTOP_CEF_SIZE) {
    fprintf(stderr, "The downloaded browser runtime failed its checksum.\n");
    ready = false;
  } else if (system::Run({"tar", "-xjf", file.string(), "-C", staging.string(), "--strip-components", "2", root + "/Release",
                          root + "/Resources"},
                         false) != 0) {
    fprintf(stderr, "The browser runtime could not be unpacked.\n");
    ready = false;
  }

  fs::remove(file, error);
  if (ready && !Installed(destination)) fs::rename(staging, destination, error);
  fs::remove_all(staging, error);
  return Installed(destination);
}

static std::string ResolveRuntime(const fs::path &application) {
  const char *override = getenv("ANT_DESKTOP_CEF");
  fs::path shared = fs::path(system::SharedRuntimeDirectory()) / ANT_DESKTOP_CEF_VERSION;
  for (const fs::path &candidate : {fs::path(override ? override : ""), application, shared}) {
    if (Installed(candidate)) return candidate.string();
  }
  return Download(shared) ? shared.string() : "";
}

int main(int argc, char **argv) {
  volatile char stack_base;
  if (argc == 2 && strncmp(argv[1], "--reload=", 9) == 0) return system::SignalReload(atoll(argv[1] + 9)) ? 0 : 1;
  fs::path application = fs::path(system::ExecutablePath()).parent_path();
  std::string runtime = ResolveRuntime(application);
  if (runtime.empty()) return 1;
  system::SetEnvironment("ANT_DESKTOP_CEF_DIR", runtime);

  void *desktop = nullptr;
  if (system::OpenLibrary((fs::path(runtime) / system::kBrowserLibrary).string())) {
    desktop = system::OpenLibrary((application / system::kDesktopLibrary).string());
  }
  auto run = reinterpret_cast<int (*)(int, char **, void *)>(desktop ? system::FindSymbol(desktop, "AntDesktop_Main") : nullptr);
  if (!run) {
    fprintf(stderr, "failed to load the Ant Desktop runtime from %s\n", application.string().c_str());
    return 1;
  }
  return run(argc, argv, const_cast<char *>(&stack_base));
}
