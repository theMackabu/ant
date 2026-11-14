#include "desktop.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

#include "include/cef_parser.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_stream_resource_handler.h"

namespace desktop {

namespace {

namespace fs = std::filesystem;

struct AppRoot {
  fs::path path;
  bool integration = false;
};

std::mutex roots_mutex;
std::map<int, AppRoot> roots;

AppRoot FindRoot(CefRefPtr<CefBrowser> browser) {
  std::lock_guard<std::mutex> lock(roots_mutex);
  auto found = browser ? roots.find(browser->GetIdentifier()) : roots.end();
  if (found == roots.end()) found = roots.find(0);
  return found == roots.end() ? AppRoot() : found->second;
}

std::string Trim(const std::string &value) {
  size_t start = value.find_first_not_of(" \t\r\n");
  size_t end = value.find_last_not_of(" \t\r\n");
  return start == std::string::npos ? "" : value.substr(start, end - start + 1);
}

std::string NamedBindings(std::string value) {
  for (size_t alias = value.find(" as "); alias != std::string::npos; alias = value.find(" as ", alias + 2)) {
    value.replace(alias, 4, ": ");
  }
  return value;
}

std::string Bind(const std::string &names, const std::string &module) {
  if (names.empty()) return module + ";";
  if (names.front() == '{') return "const " + NamedBindings(names) + " = " + module + ";";
  if (names.starts_with("* as ")) return "const " + Trim(names.substr(5)) + " = " + module + ";";
  size_t comma = names.find(',');
  if (comma == std::string::npos) return "const " + names + " = " + module + ";";
  std::string primary = Trim(names.substr(0, comma));
  std::string secondary = Trim(names.substr(comma + 1));
  secondary = secondary.starts_with("* as ") ? Trim(secondary.substr(5)) : NamedBindings(secondary);
  return "const " + primary + " = " + module + ", " + secondary + " = " + primary + ";";
}

class StringReader final : public CefReadHandler {
public:
  explicit StringReader(std::string value) : value_(std::move(value)) {}

  size_t Read(void *output, size_t size, size_t count) override {
    if (!size) return 0;
    size_t read = std::min(count, (value_.size() - position_) / size);
    memcpy(output, value_.data() + position_, read * size);
    position_ += read * size;
    return read;
  }

  int Seek(int64_t offset, int whence) override {
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? int64_t(position_) : int64_t(value_.size());
    if (base + offset < 0 || size_t(base + offset) > value_.size()) return -1;
    position_ = size_t(base + offset);
    return 0;
  }

  int64_t Tell() override { return int64_t(position_); }
  int Eof() override { return position_ >= value_.size(); }
  bool MayBlock() override { return false; }

private:
  const std::string value_;
  size_t position_ = 0;
  IMPLEMENT_REFCOUNTING(StringReader);
};

class AppSchemeFactory final : public CefSchemeHandlerFactory {
public:
  CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, const CefString &scheme,
                                       CefRefPtr<CefRequest> request) override {
    AppRoot root = FindRoot(browser);
    CefURLParts parts;
    if (root.path.empty() || !CefParseURL(request->GetURL(), parts)) return nullptr;

    cef_uri_unescape_rule_t rule = static_cast<cef_uri_unescape_rule_t>(
      UU_SPACES | UU_PATH_SEPARATORS | UU_URL_SPECIAL_CHARS_EXCEPT_PATH_SEPARATORS);
    std::string relative = CefURIDecode(CefString(&parts.path), false, rule);
    relative.erase(0, relative.find_first_not_of('/'));
    if (relative.empty()) relative = "index.html";

    std::error_code error;
    fs::path file = fs::weakly_canonical(root.path / relative, error);
    fs::path inside = file.lexically_relative(root.path);
    if (error || inside.empty() || *inside.begin() == ".." || !fs::is_regular_file(file, error)) return nullptr;

    std::string extension = file.extension().string();
    bool markup = extension == ".html" || extension == ".htm";
    if (root.integration && (markup || extension == ".js" || extension == ".mjs")) {
      std::ifstream input(file, std::ios::binary);
      std::ostringstream contents;
      contents << input.rdbuf();
      std::string source = contents.str();
      if (!input || !RewriteImports(&source, "require")) return nullptr;
      CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForHandler(new StringReader(std::move(source)));
      return new CefStreamResourceHandler(markup ? "text/html" : "text/javascript", stream);
    }

    CefRefPtr<CefStreamReader> stream = CefStreamReader::CreateForFile(file.string());
    if (!stream) return nullptr;
    CefString mime = CefGetMimeType(extension.empty() ? extension : extension.substr(1));
    return new CefStreamResourceHandler(mime.empty() ? CefString("application/octet-stream") : mime, stream);
  }

private:
  IMPLEMENT_REFCOUNTING(AppSchemeFactory);
};

}

bool RewriteImports(std::string *source, const std::string &require) {
  size_t search = 0;
  for (;;) {
    size_t import = source->find("import", search);
    if (import == std::string::npos) return true;
    size_t statement_end = std::min(source->find(';', import), source->size());
    size_t specifier = std::string::npos;
    for (const char *prefix : {"'node:", "\"node:", "'ant:", "\"ant:"}) {
      specifier = std::min(specifier, source->find(prefix, import));
    }
    if (specifier > statement_end) {
      search = import + 6;
      continue;
    }
    size_t specifier_end = source->find((*source)[specifier], specifier + 1);
    if (specifier_end == std::string::npos) return false;
    std::string module = source->substr(specifier + 1, specifier_end - specifier - 1);
    if (module.starts_with("ant://")) {
      search = specifier_end;
      continue;
    }

    std::string call = require + "('" + module + "')";
    std::string replacement;
    size_t end = source->find_first_of(";\n", specifier_end);
    if (end == std::string::npos || (*source)[end] == '\n') end = specifier_end;
    size_t from = source->rfind("from", specifier);
    if (from != std::string::npos && from >= import) {
      replacement = Bind(Trim(source->substr(import + 6, from - import - 6)), call);
    } else if (source->compare(import, 7, "import(") == 0) {
      replacement = "Promise.resolve(" + call + ")";
      end = source->find(')', specifier_end);
      if (end == std::string::npos) return false;
    } else {
      replacement = call + ";";
    }
    source->replace(import, end - import + 1, replacement);
    search = import + replacement.size();
  }
}

void RegisterSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
  registrar->AddCustomScheme(
    "ant", CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE | CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED);
}

void RegisterAppScheme() {
  CefRegisterSchemeHandlerFactory("ant", "app", new AppSchemeFactory);
}

void SetAppRoot(int browser, const std::string &root, bool integration) {
  std::error_code error;
  fs::path path = fs::weakly_canonical(root, error);
  std::lock_guard<std::mutex> lock(roots_mutex);
  if (!error) roots[browser] = {path, integration};
}

void ClearAppRoot(int browser) {
  std::lock_guard<std::mutex> lock(roots_mutex);
  roots.erase(browser);
}

}
