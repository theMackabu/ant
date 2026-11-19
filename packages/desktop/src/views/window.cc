#include "views/views.hh"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "include/cef_browser.h"
#include "include/cef_image.h"
#include "include/cef_menu_model.h"
#include "include/cef_menu_model_delegate.h"
#include "include/cef_parser.h"
#include "include/cef_request_context.h"
#include "include/views/cef_box_layout.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_display.h"
#include "include/views/cef_menu_button.h"
#include "include/views/cef_menu_button_delegate.h"
#include "include/views/cef_panel.h"
#include "include/views/cef_window.h"
#include "include/views/cef_window_delegate.h"

namespace desktop::views {

using Options = CefRefPtr<CefDictionaryValue>;

namespace {

struct MenuEntry {
  int64_t click = -1;
  std::string role;
};

struct Accelerator {
  int key = 0;
  bool shift = false;
  bool control = false;
  bool alt = false;
};

class Frame;

}

static constexpr const char *kEditRoles[] = {"undo", "redo", "cut", "copy", "paste", "selectall"};
static constexpr const char *kWindowRoles[] = {"quit", "close", "minimize", "zoom", "togglefullscreen"};

static std::map<int64_t, CefRefPtr<Frame>> frames;
static CefRefPtr<CefListValue> menu;

static bool Bool(Options options, const char *name, bool fallback) {
  return options->GetType(name) == VTYPE_BOOL ? options->GetBool(name) : fallback;
}

static double Number(Options options, const char *name, double fallback) {
  CefValueType type = options->GetType(name);
  return type == VTYPE_INT ? options->GetInt(name) : type == VTYPE_DOUBLE ? options->GetDouble(name) : fallback;
}

static std::string String(Options options, const char *name) {
  return options->GetType(name) == VTYPE_STRING ? options->GetString(name).ToString() : "";
}

static std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char unit) { return char(tolower(unit)); });
  return value;
}

template <size_t Count> static bool Contains(const char *const (&names)[Count], const std::string &name) {
  return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

static bool ParseColor(const std::string &hex, cef_color_t *color) {
  if ((hex.size() != 7 && hex.size() != 9) || hex[0] != '#') return false;
  char *end;
  unsigned long raw = strtoul(hex.c_str() + 1, &end, 16);
  if (*end) return false;
  unsigned long rgba = hex.size() == 9 ? raw : raw << 8 | 0xff;
  *color = CefColorSetARGB(rgba & 0xff, rgba >> 24 & 0xff, rgba >> 16 & 0xff, rgba >> 8 & 0xff);
  return true;
}

static Accelerator ParseAccelerator(const std::string &text) {
  Accelerator accelerator;
  std::string lower = Lower(text);
  size_t start = 0;
  for (size_t plus; (plus = lower.find('+', start)) != std::string::npos && plus + 1 < lower.size(); start = plus + 1) {
    std::string part = lower.substr(start, plus - start);
    if (part == "shift") accelerator.shift = true;
    else if (part == "alt" || part == "option") accelerator.alt = true;
    else accelerator.control = true;
  }
  std::string key = lower.substr(start);
  if (key.size() == 1 && isalnum(static_cast<unsigned char>(key[0]))) accelerator.key = toupper(key[0]);
  else if (key == "comma" || key == ",") accelerator.key = 0xbc;
  else if (key == "plus" || key == "+" || key == "=") accelerator.key = 0xbb;
  else if (key == "-") accelerator.key = 0xbd;
  else if (key == "space") accelerator.key = 0x20;
  else if (key.size() > 1 && key[0] == 'f') accelerator.key = 0x6f + atoi(key.c_str() + 1);
  return accelerator;
}

static CefRefPtr<CefImage> ApplicationIcon() {
  std::string name = ApplicationInfo("AntDesktopIcon");
  if (name.empty()) return nullptr;
  std::ifstream input(std::filesystem::path(ApplicationDirectory()) / "resources" / name, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  CefRefPtr<CefImage> image = CefImage::CreateImage();
  return image->AddPNG(1, data.data(), data.size()) ? image : nullptr;
}

namespace {

class Frame final : public CefWindowDelegate,
                    public CefBrowserViewDelegate,
                    public CefMenuButtonDelegate,
                    public CefMenuModelDelegate {
public:
  Frame(int64_t id, Options options)
      : id_(id), options_(options), show_when_ready_(Bool(options, "show", true)), closable_(Bool(options, "closable", true)) {}

  bool Host(CefRefPtr<CefClient> client, const std::string &url, const CefBrowserSettings &settings,
            CefRefPtr<CefDictionaryValue> extra) {
    if (!window_ || browser_view_) return false;
    browser_view_ = CefBrowserView::CreateBrowserView(client, url, settings, extra, CefRequestContext::GetGlobalContext(), this);
    if (!browser_view_) return false;
    window_->AddChildView(browser_view_);
    layout_->SetFlexForView(browser_view_, 1);
    window_->Layout();
    return true;
  }

  void SetRegions(const AntDesktopRegion *regions, size_t count) {
    std::vector<CefDraggableRegion> values;
    for (size_t index = 0; index < count; index++) {
      const AntDesktopRegion &region = regions[index];
      values.emplace_back(CefRect(int(region.x), int(region.y), int(region.width), int(region.height)), region.draggable);
    }
    if (window_) window_->SetDraggableRegions(values);
  }

  bool Perform(const std::string &operation, const std::string &text, bool flag) {
    if (!window_) return false;
    if (operation == "ready" && !show_when_ready_) return true;
    if (operation == "show" || operation == "ready") {
      show_when_ready_ = false;
      window_->Show();
      if (Bool(options_, "fullscreen", false) && !shown_) window_->SetFullscreen(true);
      shown_ = true;
    } else if (operation == "hide") {
      show_when_ready_ = false;
      window_->Hide();
    } else if (operation == "minimize") window_->Minimize();
    else if (operation == "restore") window_->Restore();
    else if (operation == "maximize") window_->Maximize();
    else if (operation == "setAlwaysOnTop") window_->SetAlwaysOnTop(flag);
    else if (operation == "setTitle") window_->SetTitle(text);
    else if (operation == "setFullScreen") window_->SetFullscreen(flag);
    else return false;
    return true;
  }

  bool Bounds(double *bounds) {
    if (!window_) return false;
    CefRect frame = window_->GetBounds();
    CefRect client = window_->GetClientAreaBoundsInScreen();
    CefRect work = window_->GetDisplay()->GetWorkArea();
    bounds[0] = frame.x - work.x;
    bounds[1] = frame.y - work.y;
    bounds[2] = client.width;
    bounds[3] = client.height;
    return true;
  }

  void Close() {
    closable_ = true;
    if (window_ && !window_->IsClosed()) window_->Close();
  }

  void BuildMenu() {
    if (!window_) return;
    window_->RemoveAllAccelerators();
    menu_bar_->RemoveAllChildViews();
    entries_.clear();
    models_.clear();
    for (size_t index = 0; menu && index < menu->GetSize(); index++) {
      Options item = menu->GetDictionary(index);
      CefRefPtr<CefListValue> submenu = item ? item->GetList("submenu") : nullptr;
      if (!submenu || !Bool(item, "visible", true)) continue;
      CefRefPtr<CefMenuModel> model = CefMenuModel::CreateMenuModel(this);
      Fill(model, submenu);
      if (model->GetCount() == 0) continue;
      CefRefPtr<CefMenuButton> button = CefMenuButton::CreateMenuButton(this, String(item, "label"));
      button->SetID(int(models_.size()));
      models_.push_back(model);
      menu_bar_->AddChildView(button);
    }
    menu_bar_->SetVisible(!models_.empty());
    window_->Layout();
  }

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    window_ = window;
    CefBoxLayoutSettings vertical;
    vertical.cross_axis_alignment = CEF_AXIS_ALIGNMENT_STRETCH;
    layout_ = window->SetToBoxLayout(vertical);
    CefBoxLayoutSettings horizontal;
    horizontal.horizontal = true;
    menu_bar_ = CefPanel::CreatePanel(nullptr);
    menu_bar_->SetToBoxLayout(horizontal);
    window->AddChildView(menu_bar_);

    std::string title = String(options_, "title");
    window->SetTitle(title.empty() ? ApplicationName() : title);
    cef_color_t color;
    if (ParseColor(String(options_, "backgroundColor"), &color)) window->SetBackgroundColor(color);
    if (Bool(options_, "alwaysOnTop", false)) window->SetAlwaysOnTop(true);
    if (CefRefPtr<CefImage> icon = ApplicationIcon()) {
      window->SetWindowIcon(icon);
      window->SetWindowAppIcon(icon);
    }
    system::StyleWindow(reinterpret_cast<void *>(window->GetWindowHandle()), Number(options_, "opacity", 1),
                        Bool(options_, "contentProtection", false));

    CefRect work = window->GetDisplay()->GetWorkArea();
    bool placed = options_->HasKey("x") || options_->HasKey("y");
    if (placed) window->SetPosition(CefPoint(work.x + int(Number(options_, "x", 0)), work.y + int(Number(options_, "y", 0))));
    else if (Bool(options_, "center", true)) window->CenterWindow(Size("width", "height", 900, 600));
    bounds_ = window->GetBounds();
    BuildMenu();
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    CefRefPtr<Frame> self = this;
    window_ = nullptr;
    browser_view_ = nullptr;
    menu_bar_ = nullptr;
    layout_ = nullptr;
    models_.clear();
    AntDesktop_OnWindowClosed(id_);
    frames.erase(id_);
  }

  void OnWindowBoundsChanged(CefRefPtr<CefWindow> window, const CefRect &bounds) override {
    bool moved = bounds.x != bounds_.x || bounds.y != bounds_.y;
    bool resized = bounds.width != bounds_.width || bounds.height != bounds_.height;
    bounds_ = bounds;
    if (moved) AntDesktop_OnWindowEvent(id_, "move");
    if (resized) AntDesktop_OnWindowEvent(id_, "resize");
  }

  CefRect GetInitialBounds(CefRefPtr<CefWindow> window) override {
    CefSize size = Size("width", "height", 900, 600);
    return CefRect(0, 0, size.width, size.height);
  }

  bool IsFrameless(CefRefPtr<CefWindow> window) override { return !Bool(options_, "frame", true); }
  bool CanResize(CefRefPtr<CefWindow> window) override { return Bool(options_, "resizable", true); }
  bool CanMaximize(CefRefPtr<CefWindow> window) override { return Bool(options_, "maximizable", true); }
  bool CanMinimize(CefRefPtr<CefWindow> window) override { return Bool(options_, "minimizable", true); }
  bool CanClose(CefRefPtr<CefWindow> window) override { return closable_ && AntDesktop_OnWindowShouldClose(id_); }
  cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }
  cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

  CefSize GetMinimumSize(CefRefPtr<CefView> view) override {
    return view->AsPanel() && view->AsPanel()->AsWindow() ? Size("minWidth", "minHeight", 0, 0) : CefSize();
  }

  CefSize GetMaximumSize(CefRefPtr<CefView> view) override {
    return view->AsPanel() && view->AsPanel()->AsWindow() ? Size("maxWidth", "maxHeight", 0, 0) : CefSize();
  }

  bool OnAccelerator(CefRefPtr<CefWindow> window, int command) override {
    return Execute(command);
  }

  void OnButtonPressed(CefRefPtr<CefButton> button) override {}

  void OnMenuButtonPressed(CefRefPtr<CefMenuButton> button, const CefPoint &point,
                           CefRefPtr<CefMenuButtonPressedLock> lock) override {
    size_t index = size_t(button->GetID());
    if (index < models_.size()) button->ShowMenu(models_[index], point, CEF_MENU_ANCHOR_TOPLEFT);
  }

  void ExecuteCommand(CefRefPtr<CefMenuModel> model, int command, cef_event_flags_t flags) override {
    Execute(command);
  }

private:
  CefSize Size(const char *width, const char *height, int default_width, int default_height) const {
    return CefSize(std::max(0, int(Number(options_, width, default_width))), std::max(0, int(Number(options_, height, default_height))));
  }

  void Fill(CefRefPtr<CefMenuModel> model, CefRefPtr<CefListValue> items) {
    for (size_t index = 0; index < items->GetSize(); index++) {
      Options item = items->GetDictionary(index);
      if (!item) continue;
      if (String(item, "type") == "separator") {
        model->AddSeparator();
        continue;
      }
      std::string role = Lower(String(item, "role"));
      bool clickable = item->GetType("id") == VTYPE_INT;
      CefRefPtr<CefListValue> submenu = item->GetList("submenu");
      if (!clickable && !submenu && !Contains(kEditRoles, role) && !Contains(kWindowRoles, role)) continue;

      int command = MENU_ID_USER_FIRST + int(entries_.size());
      entries_.push_back({clickable ? item->GetInt("id") : -1, role});
      std::string label = String(item, "label");
      if (label.empty()) label = role;
      if (submenu) Fill(model->AddSubMenu(command, label), submenu);
      else if (item->HasKey("checked")) model->AddCheckItem(command, label);
      else model->AddItem(command, label);
      model->SetEnabled(command, Bool(item, "enabled", true));
      model->SetVisible(command, Bool(item, "visible", true));
      model->SetChecked(command, Bool(item, "checked", false));

      Accelerator accelerator = ParseAccelerator(String(item, "accelerator"));
      if (accelerator.key == 0) continue;
      model->SetAccelerator(command, accelerator.key, accelerator.shift, accelerator.control, accelerator.alt);
      if (Contains(kEditRoles, role)) continue;
      window_->SetAccelerator(command, accelerator.key, accelerator.shift, accelerator.control, accelerator.alt, true);
    }
  }

  bool Execute(int command) {
    size_t index = size_t(command - MENU_ID_USER_FIRST);
    if (index >= entries_.size()) return false;
    MenuEntry entry = entries_[index];
    if (entry.click >= 0) AntDesktop_OnMenuClick(entry.click);
    else if (entry.role == "quit") AntDesktop_OnTerminate();
    else if (!window_) return false;
    else if (entry.role == "close") window_->Close();
    else if (entry.role == "minimize") window_->Minimize();
    else if (entry.role == "zoom") window_->IsMaximized() ? window_->Restore() : window_->Maximize();
    else if (entry.role == "togglefullscreen") window_->SetFullscreen(!window_->IsFullscreen());
    else return Edit(entry.role);
    return true;
  }

  bool Edit(const std::string &role) {
    CefRefPtr<CefBrowser> browser = browser_view_ ? browser_view_->GetBrowser() : nullptr;
    CefRefPtr<CefFrame> frame = browser ? browser->GetFocusedFrame() : nullptr;
    if (!frame) return false;
    if (role == "undo") frame->Undo();
    else if (role == "redo") frame->Redo();
    else if (role == "cut") frame->Cut();
    else if (role == "copy") frame->Copy();
    else if (role == "paste") frame->Paste();
    else if (role == "selectall") frame->SelectAll();
    else return false;
    return true;
  }

  const int64_t id_;
  const Options options_;
  CefRefPtr<CefWindow> window_;
  CefRefPtr<CefBoxLayout> layout_;
  CefRefPtr<CefPanel> menu_bar_;
  CefRefPtr<CefBrowserView> browser_view_;
  std::vector<CefRefPtr<CefMenuModel>> models_;
  std::vector<MenuEntry> entries_;
  CefRect bounds_;
  bool show_when_ready_;
  bool closable_;
  bool shown_ = false;
  IMPLEMENT_REFCOUNTING(Frame);
};

}

static CefRefPtr<Frame> Find(int64_t id) {
  auto found = frames.find(id);
  return found == frames.end() ? nullptr : found->second;
}

void SetMenu(const char *json) {
  CefRefPtr<CefValue> parsed = json ? CefParseJSON(json, JSON_PARSER_RFC) : nullptr;
  menu = parsed ? parsed->GetList() : nullptr;
  for (const auto &entry : frames) entry.second->BuildMenu();
}

}

namespace desktop::platform {

bool CreateBrowser(int64_t window, CefRefPtr<CefClient> client, const std::string &url, const CefBrowserSettings &settings,
                   CefRefPtr<CefDictionaryValue> extra) {
  CefRefPtr<views::Frame> frame = views::Find(window);
  return frame && frame->Host(client, url, settings, extra);
}

void AttachBrowser(int64_t window, CefRefPtr<CefBrowser> browser) {}

}

using namespace desktop;

bool AntDesktop_Window_Create(int64_t window, const char *options_json) {
  CefRefPtr<CefValue> parsed = CefParseJSON(options_json, JSON_PARSER_RFC);
  views::Options options = parsed ? parsed->GetDictionary() : nullptr;
  if (!options) options = CefDictionaryValue::Create();
  CefRefPtr<views::Frame> frame = new views::Frame(window, options);
  views::frames[window] = frame;
  CefWindow::CreateTopLevelWindow(frame);
  return true;
}

void AntDesktop_Window_SetRegions(int64_t window, const AntDesktopRegion *regions, size_t count) {
  if (CefRefPtr<views::Frame> frame = views::Find(window)) frame->SetRegions(regions, count);
}

bool AntDesktop_Window_Perform(int64_t window, const char *operation, const char *text, bool flag) {
  CefRefPtr<views::Frame> frame = views::Find(window);
  return frame && frame->Perform(operation, text, flag);
}

bool AntDesktop_Window_Bounds(int64_t window, double *bounds) {
  CefRefPtr<views::Frame> frame = views::Find(window);
  return frame && frame->Bounds(bounds);
}

void AntDesktop_Window_Close(int64_t window) {
  if (CefRefPtr<views::Frame> frame = views::Find(window)) frame->Close();
}
