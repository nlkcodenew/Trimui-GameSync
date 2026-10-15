// main.cpp — Trimui-GameSync P0 skeleton.
// C++17 + SDL2 + SDL_ttf + libcurl. Gamepad-first UI, uniform scaling for
// 4:3 (1024x768 Brick Pro) and 16:9 (1280x720 Smart Pro S).
#include "AppConfig.h"
#include "Drive.h"
#include "Fixer.h"
#include "Logger.h"
#include "OtaVerify.h"
#include "Scanner.h"
#include "Sync.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <dirent.h>
#include <dlfcn.h>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace GameSync {

constexpr int kBaseW = 1024;
constexpr int kBaseH = 768;

// Real screen size (detected at runtime) + uniform UI scale.
// All UI coordinates are authored in the 1024x768 logical space and mapped
// through X()/Y() with a UNIFORM scale (no stretched text on 16:9),
// centered on the real screen. Backgrounds are drawn full-bleed.
int scrW = kBaseW;
int scrH = kBaseH;
float uiScale = 1.0f;
int uiOX = 0;
int uiOY = 0;

inline int X(int x) { return uiOX + static_cast<int>(x * uiScale); }
inline int Y(int y) { return uiOY + static_cast<int>(y * uiScale); }
inline int headerH() { return Y(64); }
inline int footerH() { return Y(53); }

enum class Screen {
  Home, Library, Drive, Sync, Fix, Settings, About, Donate
};

struct InputState {
  bool up = false;
  bool down = false;
  bool left = false;
  bool right = false;
  bool a = false;
  bool b = false;
  bool x = false;
  bool y = false;
  bool start = false;
};

SDL_Window *window = nullptr;
SDL_Renderer *renderer = nullptr;
TTF_Font *font = nullptr;
TTF_Font *fontSmall = nullptr;
SDL_GameController *controller = nullptr;
SDL_Joystick *joystick = nullptr;
std::string appDirectory = ".";
std::string appVersion = "0.1.0";
std::atomic<bool> running{true};
Screen screen = Screen::Home;
int menuSelection = 0;

SDL_Color color(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255) {
  return SDL_Color{red, green, blue, alpha};
}

void fillRect(int x, int y, int width, int height, SDL_Color value) {
  SDL_SetRenderDrawColor(renderer, value.r, value.g, value.b, value.a);
  SDL_Rect rect{x, y, width, height};
  SDL_RenderFillRect(renderer, &rect);
}

std::string truncateText(const std::string &text, TTF_Font *textFont,
                         int maxWidth) {
  int width = 0;
  if (TTF_SizeUTF8(textFont, text.c_str(), &width, nullptr) == 0 &&
      width <= maxWidth) {
    return text;
  }
  std::string output;
  size_t position = 0;
  while (position < text.size()) {
    size_t next = position + 1;
    const unsigned char lead = static_cast<unsigned char>(text[position]);
    if ((lead & 0xE0) == 0xC0) next = position + 2;
    else if ((lead & 0xF0) == 0xE0) next = position + 3;
    else if ((lead & 0xF8) == 0xF0) next = position + 4;
    next = std::min(next, text.size());
    const std::string candidate =
        output + text.substr(position, next - position) + "...";
    if (TTF_SizeUTF8(textFont, candidate.c_str(), &width, nullptr) != 0 ||
        width > maxWidth) {
      return output.empty() ? "..." : output + "...";
    }
    output.append(text, position, next - position);
    position = next;
  }
  return output;
}

void drawText(const std::string &text, int x, int y, SDL_Color value,
              TTF_Font *textFont = nullptr, int maxWidth = 0) {
  if (!textFont) textFont = font;
  if (!textFont) return;
  const std::string shown =
      maxWidth > 0 ? truncateText(text, textFont, maxWidth) : text;
  SDL_Surface *surface = TTF_RenderUTF8_Blended(textFont, shown.c_str(), value);
  if (!surface) return;
  SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, surface);
  if (texture) {
    SDL_Rect destination{x, y, surface->w, surface->h};
    SDL_RenderCopy(renderer, texture, nullptr, &destination);
    SDL_DestroyTexture(texture);
  }
  SDL_FreeSurface(surface);
}

void drawFooter(const std::string &left, const std::string &right) {
  const int top = scrH - footerH();
  fillRect(0, top, scrW, footerH(), color(20, 24, 31));
  fillRect(0, top, scrW, std::max(1, Y(1)), color(55, 64, 78));
  drawText(left, X(28), top + Y(15), color(220, 225, 232), fontSmall, X(660));
  int width = 0;
  if (TTF_SizeUTF8(fontSmall, right.c_str(), &width, nullptr) == 0) {
    drawText(right, scrW - width - X(28), top + Y(15), color(220, 225, 232),
             fontSmall);
  }
}

// ---------------- NLK intro (ported from Trimui-Remote ui/remote-ui.c) ----------------
// 2.2s splash: dark background (8,8,12), giant red (229,9,20) "NLK" letters
// rising one by one with overshoot bounce + drop shadow + white sweep.
// Skip with any key. Disable via GS_NO_INTRO=1, intro.off / .no-intro file,
// or "intro": false in config.

bool introDisabled(const std::string &appdir) {
  const char *env = getenv("GS_NO_INTRO");
  if (env && strcmp(env, "1") == 0) return true;
  if (access((appdir + "/intro.off").c_str(), F_OK) == 0) return true;
  if (access((appdir + "/.no-intro").c_str(), F_OK) == 0) return true;
  if (access((appdir + "/data/.no-intro").c_str(), F_OK) == 0)
    return true;
  std::ifstream cfg(appdir + "/config.json", std::ios::binary);
  if (!cfg) return false;
  std::string buf((std::istreambuf_iterator<char>(cfg)),
                  std::istreambuf_iterator<char>());
  const size_t key = buf.find("\"intro\"");
  if (key == std::string::npos) return false;
  const size_t colon = buf.find(':', key);
  if (colon == std::string::npos) return false;
  size_t i = colon + 1;
  while (i < buf.size() && (buf[i] == ' ' || buf[i] == '\t')) ++i;
  return buf.compare(i, 5, "false") == 0;
}

double introSpread(double progress, double k) {
  double ease = progress / 0.55;
  if (ease < 0.0) ease = 0.0;
  if (ease > 1.0) ease = 1.0;
  return (4.0 + 26.0 * (1.0 - (1.0 - ease) * (1.0 - ease))) * k;
}

void playIntro(SDL_Renderer *ren, int W, int H, const std::string &fontpath,
               const std::string &appdir) {
  if (introDisabled(appdir)) return;
  if (!ren || W <= 0 || H <= 0) return;
  double s = W / 1024.0;
  const double sy = H / 768.0;
  if (sy < s) s = sy;
  if (s < 0.75) s = 0.75;
  int giant = static_cast<int>(132.0 * s + 0.5);
  if (giant < 24) giant = 24;
  double k = giant / 132.0;
  TTF_Font *gfont = TTF_OpenFont(fontpath.c_str(), giant);
  if (!gfont) {
    static const int smaller[] = {260, 220, 190, 160, 132, 100};
    for (size_t i = 0; i < sizeof(smaller) / sizeof(smaller[0]) && !gfont; ++i) {
      if (smaller[i] >= giant) continue;
      gfont = TTF_OpenFont(fontpath.c_str(), smaller[i]);
      if (gfont) {
        giant = smaller[i];
        k = giant / 132.0;
      }
    }
  }
  if (!gfont) return;
  const SDL_Color dark = {60, 5, 8, 255};
  const SDL_Color bright = {229, 9, 20, 255};
  const SDL_Color white = {255, 255, 255, 255};
  static const char *letters = "NLK";
  SDL_Texture *tex_dark[3] = {nullptr, nullptr, nullptr};
  SDL_Texture *tex_bright[3] = {nullptr, nullptr, nullptr};
  SDL_Texture *tex_white[3] = {nullptr, nullptr, nullptr};
  int gw[3] = {0, 0, 0}, gh[3] = {0, 0, 0};
  for (int i = 0; i < 3; i++) {
    char ch[2] = {letters[i], '\0'};
    SDL_Surface *sd = TTF_RenderUTF8_Blended(gfont, ch, dark);
    SDL_Surface *sb = TTF_RenderUTF8_Blended(gfont, ch, bright);
    SDL_Surface *sw = TTF_RenderUTF8_Blended(gfont, ch, white);
    if (sb) {
      gw[i] = sb->w;
      gh[i] = sb->h;
    }
    if (sd) {
      tex_dark[i] = SDL_CreateTextureFromSurface(ren, sd);
      SDL_FreeSurface(sd);
    }
    if (sb) {
      tex_bright[i] = SDL_CreateTextureFromSurface(ren, sb);
      SDL_FreeSurface(sb);
    }
    if (sw) {
      tex_white[i] = SDL_CreateTextureFromSurface(ren, sw);
      SDL_FreeSurface(sw);
    }
  }
  TTF_CloseFont(gfont);
  int usable = 0;
  for (int i = 0; i < 3; i++)
    if (tex_bright[i]) usable++;
  if (usable < 3) {
    for (int i = 0; i < 3; i++) {
      if (tex_dark[i]) SDL_DestroyTexture(tex_dark[i]);
      if (tex_bright[i]) SDL_DestroyTexture(tex_bright[i]);
      if (tex_white[i]) SDL_DestroyTexture(tex_white[i]);
    }
    return;
  }
  const double duration_ms = 2200.0;
  const Uint32 start = SDL_GetTicks();
  SDL_Event ev;
  const int center_y = H / 2;
  const double total_glyph = gw[0] + gw[1] + gw[2];
  bool skipped = false;
  while (!skipped) {
    const Uint32 elapsed = SDL_GetTicks() - start;
    if (static_cast<double>(elapsed) >= duration_ms) break;
    const double progress = static_cast<double>(elapsed) / duration_ms;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_QUIT || ev.type == SDL_KEYDOWN ||
          ev.type == SDL_JOYBUTTONDOWN || ev.type == SDL_JOYHATMOTION) {
        skipped = true;
        break;
      }
    }
    if (skipped) break;
    const double spacing = introSpread(progress, k);
    const double total = total_glyph + spacing * 2.0;
    double fit = 1.0;
    if (total > 0.0 && (W - 80) / total < fit) fit = (W - 80) / total;
    if (fit < 0.05) fit = 0.05;
    int cursor = static_cast<int>((W - total * fit) / 2.0);
    SDL_SetRenderDrawColor(ren, 8, 8, 12, 255);
    SDL_RenderClear(ren);
    for (int i = 0; i < 3; i++) {
      const int dw = static_cast<int>(gw[i] * fit);
      const int dh = static_cast<int>(gh[i] * fit);
      const int x = cursor + static_cast<int>((gw[i] * fit - dw) / 2.0);
      const double enter_at = 0.05 + i * 0.16;
      double local = (progress - enter_at) / 0.30;
      if (local > 0.0) {
        if (local > 1.0) local = 1.0;
        int rise = static_cast<int>((1.0 - local) * 90.0 * k);
        if (local > 0.65) {
          const double sn =
              sin((local - 0.65) / 0.35 * 3.14159265358979323846);
          rise += static_cast<int>(-14.0 * k * sn);
        }
        const int y = center_y - dh / 2 + rise;
        SDL_Texture *tex =
            (local * 1.5 >= 0.75) ? tex_bright[i] : tex_dark[i];
        if (local * 1.5 >= 0.75 && tex_dark[i]) {
          const SDL_Rect glow = {x + static_cast<int>(4.0 * fit * k),
                                 y + static_cast<int>(6.0 * fit * k), dw, dh};
          SDL_RenderCopy(ren, tex_dark[i], nullptr, &glow);
        }
        const SDL_Rect r = {x, y, dw, dh};
        SDL_RenderCopy(ren, tex, nullptr, &r);
      }
      cursor += static_cast<int>((gw[i] + spacing) * fit);
    }
    if (progress > 0.72) {
      const double sweep = (progress - 0.72) / 0.28;
      cursor = static_cast<int>((W - total * fit) / 2.0);
      for (int i = 0; i < 3; i++) {
        const int dw = static_cast<int>(gw[i] * fit);
        const int dh = static_cast<int>(gh[i] * fit);
        const double center = i / 2.0;
        if (tex_white[i] && fabs(sweep - center * 0.9) < 0.18) {
          const SDL_Rect r = {
              cursor + static_cast<int>((gw[i] * fit - dw) / 2.0),
              center_y - dh / 2, dw, dh};
          SDL_RenderCopy(ren, tex_white[i], nullptr, &r);
        }
        cursor += static_cast<int>((gw[i] + spacing) * fit);
      }
    }
    SDL_RenderPresent(ren);
    SDL_Delay(16);
  }
  for (int i = 0; i < 3; i++) {
    if (tex_dark[i]) SDL_DestroyTexture(tex_dark[i]);
    if (tex_bright[i]) SDL_DestroyTexture(tex_bright[i]);
    if (tex_white[i]) SDL_DestroyTexture(tex_white[i]);
  }
  while (SDL_PollEvent(&ev)) { /* drain keys pressed during intro */
  }
}

// ---------------- version ----------------

std::string readRuntimeVersion(const std::string &appdir) {
  std::ifstream f(appdir + "/VERSION", std::ios::binary);
  std::string v;
  std::getline(f, v);
  while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' '))
    v.pop_back();
  size_t b = 0;
  while (b < v.size() && (v[b] == ' ' || v[b] == '\t')) ++b;
  v = v.substr(b);
  return v.empty() ? APP_VERSION : v;
}

// ---------------- input ----------------

void moveSelection(int delta, int itemCount, int &selection) {
  if (itemCount <= 0) {
    selection = 0;
    return;
  }
  selection = (selection + delta + itemCount) % itemCount;
}

InputState pollInput() {
  InputState input;
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) running = false;
    if (event.type == SDL_KEYDOWN && event.key.repeat == 0) {
      switch (event.key.keysym.sym) {
      case SDLK_UP: input.up = true; break;
      case SDLK_DOWN: input.down = true; break;
      case SDLK_LEFT: input.left = true; break;
      case SDLK_RIGHT: input.right = true; break;
      case SDLK_RETURN: case SDLK_a: input.a = true; break;
      case SDLK_ESCAPE: case SDLK_b: input.b = true; break;
      case SDLK_x: input.x = true; break;
      case SDLK_y: input.y = true; break;
      case SDLK_s: input.start = true; break;
      default: break;
      }
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN) {
      switch (event.cbutton.button) {
      case SDL_CONTROLLER_BUTTON_DPAD_UP: input.up = true; break;
      case SDL_CONTROLLER_BUTTON_DPAD_DOWN: input.down = true; break;
      case SDL_CONTROLLER_BUTTON_DPAD_LEFT: input.left = true; break;
      case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input.right = true; break;
      case SDL_CONTROLLER_BUTTON_B: input.a = true; break;
      case SDL_CONTROLLER_BUTTON_A: input.b = true; break;
      case SDL_CONTROLLER_BUTTON_Y: input.x = true; break;
      case SDL_CONTROLLER_BUTTON_X: input.y = true; break;
      case SDL_CONTROLLER_BUTTON_START: input.start = true; break;
      default: break;
      }
    }
    if (event.type == SDL_JOYBUTTONDOWN) {
      switch (event.jbutton.button) {
      case 1: input.a = true; break;
      case 0: input.b = true; break;
      case 3: input.x = true; break;
      case 2: input.y = true; break;
      case 7: input.start = true; break;
      default: break;
      }
    }
    if (event.type == SDL_JOYHATMOTION) {
      input.up = (event.jhat.value & SDL_HAT_UP) != 0;
      input.down = (event.jhat.value & SDL_HAT_DOWN) != 0;
      input.left = (event.jhat.value & SDL_HAT_LEFT) != 0;
      input.right = (event.jhat.value & SDL_HAT_RIGHT) != 0;
    }
  }
  return input;
}

// ================= P1: data + UI state =================

struct SystemInfo {
  std::string driveName;
  std::string localCode;
  bool unsupported = false;
};

enum class KbMode { None, Search, AddLink, AddApiKey };

struct VKey {
  bool open = false;
  std::string title;
  std::string buffer;
  int row = 0, col = 0;
  bool done = false;
  bool cancelled = false;
};

struct Dialog {
  bool open = false;
  std::string title;
  std::string body;
};

const char *kKeyRows[] = {
    "ABCDEFGHIJKLM",
    "NOPQRSTUVWXYZ",
    "abcdefghijklm",
    "nopqrstuvwxyz",
    "0123456789",
    ":/.?=&-_#@ ",
};
const int kKeyRowCount = 6;

// (P5: extension -> system mapping table lives in src/Fixer.* —
// extMapDisplayRows() is the single source of truth for the Fix screen.)

const char *kSyncTabs[] = {"Tất cả", "↓ Máy thiếu", "↑ Drive thiếu",
                           "≠ Khác bản"};
const int kSyncTabCount = 4;

struct HomeAction {
  const char *title;
  const char *subtitle;
  Screen target;
};
const HomeAction kHomeActions[] = {
    {"Quét thư viện", "Duyệt game đang có trên máy", Screen::Library},
    {"So sánh", "Đối chiếu game máy ↔ Drive", Screen::Sync},
    {"Đồng bộ", "Tải game từ Drive về máy", Screen::Sync},
    {"Sửa sai thư mục", "Di chuyển file đặt nhầm chỗ", Screen::Fix},
};
const int kHomeActionCount = sizeof(kHomeActions) / sizeof(kHomeActions[0]);

// ---- global UI state ----
std::vector<SystemInfo> gSystems;
std::vector<std::string> gDriveLinks;
int homeSel = 0;
int libTab = 0, libTabScroll = 0;
bool libSearching = false;
std::string libQuery;
int driveSel = 0, driveScroll = 0;
int syncTab = 1;  // mặc định tab "Máy thiếu" (kiểu RomCloud)
std::set<std::string> syncMarked;  // key: system + "/" + relPath
// ---------------- P4: compare + download queue state ----------------
std::vector<DiffEntry> gDiff;
std::mutex gDiffMutex;
std::atomic<bool> syncDirty{true};
int syncSel = 0, syncScroll = 0;
enum class SyncMode { Compare, Queue };
SyncMode syncMode = SyncMode::Compare;
std::unique_ptr<DownloadQueue> gQueue;
bool queueDoneHandled = true;
bool queueAuthShown = false;

struct ConfirmDlg {
  bool open = false;
  std::string title;
  std::string body;
  std::function<void()> onYes;
};
ConfirmDlg cfm;

// ---------------- P5: fix misplaced files ----------------
std::vector<FixEntry> gFix;
std::mutex gFixMutex;
std::set<std::string> fixMarked;  // key: system + "/" + relPath
int fixSel = 0;
int fixScroll = 0;
bool fixDirty = true;

std::string fixRowKey(const FixEntry &e) { return e.system + "/" + e.relPath; }

// Forward declarations (defined after startScan, used by drawFix/input).
void rebuildFix();
void doFixApply(std::vector<FixEntry> entries);

int settingsSel = 0;
int aboutSel = 0;
VKey vkey;
KbMode kbMode = KbMode::None;
Dialog dlg;
SDL_Texture *qrTex = nullptr;
int qrW = 0, qrH = 0;
bool qrTried = false, qrOk = false;
// ---------------- P2: library scan state ----------------
ScanResult gScan;
std::mutex gScanMutex;
std::atomic<bool> scanRunning{false};
std::atomic<uint64_t> scanProgress{0};
std::atomic<bool> scanDone{false};
std::atomic<bool> scanFinishedFlag{false};
std::atomic<bool> gScanCancel{false};
// ---------------- P3: Drive network state ----------------
std::atomic<bool> driveNetRunning{false};
std::atomic<uint64_t> driveNetProgress{0};
std::atomic<bool> driveNetDone{false};
std::atomic<bool> driveNetCancel{false};  // B hủy khi đang quét Drive
std::atomic<bool> driveNetWasRescan{false};  // true neu op vua xong la rescan
std::atomic<uint64_t> driveNetRescanFiles{0};  // tong file rescan vua xong
std::mutex driveNetMutex;
std::string driveNetTitle, driveNetMsg;  // guarded by driveNetMutex
std::atomic<bool> pendingFullSync{false};  // chuỗi "Đồng bộ tất cả": scan → drive → so sánh
// P3 forward declarations (defined before the input section)
void startScan();
void startDriveRescan();
std::string maskApiKey(const std::string &k);
uint64_t driveCachedCount(const std::string &folderId);
std::atomic<uint64_t> scanFileCount{0};
std::atomic<uint64_t> scanTotalBytes{0};

struct LibTab {
  std::string code;  // local system code, "" = "Khác" (unknown codes)
  std::string label;
  std::string driveName;
  bool unsupported = false;
  int count = 0;
};
std::vector<LibTab> libTabs;  // systems with games, gSystems order + "Khác"
std::vector<size_t> libView;  // indices into gScan.files for tab+query
int libSel = 0, libScroll = 0;

// ---------------- small helpers ----------------

bool fileExists(const std::string &p) { return access(p.c_str(), F_OK) == 0; }

std::string readWholeFile(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return "";
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}

// Minimal parse of {"mapping": {"Drive name": "CODE", ...}}.
std::vector<SystemInfo> loadSystems(const std::string &appdir) {
  std::vector<SystemInfo> out;
  const std::string buf = readWholeFile(appdir + "/config/system_map.json");
  const size_t mpos = buf.find("\"mapping\"");
  if (mpos == std::string::npos) return out;
  size_t pos = buf.find('{', mpos);
  if (pos == std::string::npos) return out;
  ++pos;
  while (pos < buf.size()) {
    const size_t q1 = buf.find('"', pos);
    if (q1 == std::string::npos) break;
    const size_t q2 = buf.find('"', q1 + 1);
    if (q2 == std::string::npos) break;
    const size_t c = buf.find(':', q2);
    if (c == std::string::npos) break;
    const size_t q3 = buf.find('"', c);
    if (q3 == std::string::npos) break;
    const size_t q4 = buf.find('"', q3 + 1);
    if (q4 == std::string::npos) break;
    SystemInfo s;
    s.driveName = buf.substr(q1 + 1, q2 - q1 - 1);
    s.localCode = buf.substr(q3 + 1, q4 - q3 - 1);
    s.unsupported = (s.localCode == "GAMECUBE");
    out.push_back(s);
    pos = q4 + 1;
    const size_t comma = buf.find(',', pos);
    const size_t brace = buf.find('}', pos);
    if (brace != std::string::npos &&
        (comma == std::string::npos || brace < comma))
      break;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return out;
}

std::vector<std::string> loadDriveLinks(const std::string &appdir) {
  std::vector<std::string> out;
  std::ifstream f(appdir + "/data/DriveLinks.txt");
  std::string line;
  while (std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t b = 0;
    while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
    line = line.substr(b);
    if (line.empty() || line[0] == '#') continue;
    out.push_back(line);
  }
  return out;
}

std::string trimStr(std::string s) {
  size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' ||
                          s[b] == '\n'))
    ++b;
  size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' ||
                   s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

std::string shortMiddle(const std::string &s, size_t keep = 46) {
  if (s.size() <= keep) return s;
  return s.substr(0, keep - 3) + "...";
}

// ---------------- P2: accent-insensitive search + library view ----------------

// Decode one UTF-8 code point. Returns bytes consumed, cp set.
size_t utf8Decode(const char *s, size_t n, uint32_t &cp) {
  if (n == 0) return 0;
  const unsigned char c0 = static_cast<unsigned char>(s[0]);
  if (c0 < 0x80) {
    cp = c0;
    return 1;
  }
  if ((c0 & 0xE0) == 0xC0 && n >= 2) {
    cp = (static_cast<uint32_t>(c0 & 0x1F) << 6) |
         (static_cast<unsigned char>(s[1]) & 0x3F);
    return 2;
  }
  if ((c0 & 0xF0) == 0xE0 && n >= 3) {
    cp = (static_cast<uint32_t>(c0 & 0x0F) << 12) |
         ((static_cast<unsigned char>(s[1]) & 0x3F) << 6) |
         (static_cast<unsigned char>(s[2]) & 0x3F);
    return 3;
  }
  if ((c0 & 0xF8) == 0xF0 && n >= 4) {
    cp = (static_cast<uint32_t>(c0 & 0x07) << 18) |
         ((static_cast<unsigned char>(s[1]) & 0x3F) << 12) |
         ((static_cast<unsigned char>(s[2]) & 0x3F) << 6) |
         (static_cast<unsigned char>(s[3]) & 0x3F);
    return 4;
  }
  cp = c0;
  return 1;
}

// Vietnamese diacritics (both cases) -> lowercase ASCII base letter.
uint32_t foldViChar(uint32_t cp) {
  if (cp >= 'A' && cp <= 'Z') return cp + 32;
  switch (cp) {
  case 0xC0: case 0xC1: case 0xE0: case 0xE1: case 0x1EA0: case 0x1EA1:
  case 0x1EA2: case 0x1EA3: case 0xC3: case 0xE3: case 0xC2: case 0xE2:
  case 0x1EA6: case 0x1EA7: case 0x1EA4: case 0x1EA5: case 0x1EAC: case 0x1EAD:
  case 0x1EA8: case 0x1EA9: case 0x1EAA: case 0x1EAB: case 0x102: case 0x103:
  case 0x1EB0: case 0x1EB1: case 0x1EAE: case 0x1EAF: case 0x1EB6: case 0x1EB7:
  case 0x1EB2: case 0x1EB3: case 0x1EB4: case 0x1EB5:
    return 'a';
  case 0xC8: case 0xC9: case 0xE8: case 0xE9: case 0x1EB8: case 0x1EB9:
  case 0x1EBA: case 0x1EBB: case 0xCA: case 0xEA: case 0x1EC0: case 0x1EC1:
  case 0x1EBE: case 0x1EBF: case 0x1EC6: case 0x1EC7: case 0x1EC2: case 0x1EC3:
  case 0x1EC4: case 0x1EC5:
    return 'e';
  case 0xCC: case 0xCD: case 0xEC: case 0xED: case 0x1ECA: case 0x1ECB:
  case 0x1EC8: case 0x1EC9: case 0x128: case 0x129:
    return 'i';
  case 0xD2: case 0xD3: case 0xF2: case 0xF3: case 0x1ECC: case 0x1ECD:
  case 0x1ECE: case 0x1ECF: case 0xD5: case 0xF5: case 0xD4: case 0xF4:
  case 0x1ED2: case 0x1ED3: case 0x1ED0: case 0x1ED1: case 0x1ED8: case 0x1ED9:
  case 0x1ED4: case 0x1ED5: case 0x1ED6: case 0x1ED7: case 0x1A0: case 0x1A1:
  case 0x1EDC: case 0x1EDD: case 0x1EDA: case 0x1EDB: case 0x1EE2: case 0x1EE3:
  case 0x1EDE: case 0x1EDF: case 0x1EE0: case 0x1EE1:
    return 'o';
  case 0xD9: case 0xDA: case 0xF9: case 0xFA: case 0x1EE4: case 0x1EE5:
  case 0x1EE6: case 0x1EE7: case 0x168: case 0x169: case 0x1B0: case 0x1B1:
  case 0x1EEA: case 0x1EEB: case 0x1EE8: case 0x1EE9: case 0x1EF0: case 0x1EF1:
  case 0x1EEC: case 0x1EED: case 0x1EEE: case 0x1EEF:
    return 'u';
  case 0x1EF2: case 0x1EF3: case 0xDD: case 0xFD: case 0x1EF4: case 0x1EF5:
  case 0x1EF6: case 0x1EF7: case 0x178: case 0xFF:
    return 'y';
  case 0x110: case 0x111:
    return 'd';
  default:
    return cp;
  }
}

std::string foldVi(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    uint32_t cp = 0;
    const size_t n = utf8Decode(s.data() + i, s.size() - i, cp);
    if (n == 0) break;
    const uint32_t f = foldViChar(cp);
    if (f < 0x80) {
      out.push_back(static_cast<char>(f));
    } else {
      out.append(s, i, n);  // keep other scripts byte-identical
    }
    i += n;
  }
  return out;
}

bool isKnownSystem(const std::string &code) {
  for (const auto &s : gSystems)
    if (s.localCode == code) return true;
  return false;
}

// Tabs = systems that have games (gSystems order) + "Khác" for unknown codes.
// UI thread only.
void rebuildLibTabs() {
  std::map<std::string, int> counts;
  {
    std::lock_guard<std::mutex> lk(gScanMutex);
    for (const auto &f : gScan.files) counts[f.system]++;
  }
  libTabs.clear();
  for (const auto &s : gSystems) {
    const auto it = counts.find(s.localCode);
    if (it != counts.end() && it->second > 0) {
      LibTab t;
      t.code = s.localCode;
      t.label = s.localCode + (s.unsupported ? " !" : "");
      t.driveName = s.driveName;
      t.unsupported = s.unsupported;
      t.count = it->second;
      libTabs.push_back(t);
    }
  }
  int other = 0;
  for (const auto &kv : counts)
    if (!isKnownSystem(kv.first)) other += kv.second;
  if (other > 0) {
    LibTab t;
    t.code = "";
    t.label = "Kh\u00e1c";
    t.count = other;
    libTabs.push_back(t);
  }
  if (libTab >= static_cast<int>(libTabs.size())) libTab = 0;
  if (libTab < 0) libTab = 0;
}

// Filtered indices for the current tab + search query. UI thread only.
void refreshLibView() {
  libView.clear();
  libSel = 0;
  libScroll = 0;
  if (!scanDone || libTabs.empty()) return;
  if (libTab < 0 || libTab >= static_cast<int>(libTabs.size())) libTab = 0;
  const std::string code = libTabs[libTab].code;
  const std::string q = foldVi(trimStr(libQuery));
  const bool useQ = libSearching && !q.empty();
  std::lock_guard<std::mutex> lk(gScanMutex);
  for (size_t i = 0; i < gScan.files.size(); ++i) {
    const GameFile &f = gScan.files[i];
    if (code.empty()) {
      if (isKnownSystem(f.system)) continue;
    } else if (f.system != code) {
      continue;
    }
    if (useQ && foldVi(f.name).find(q) == std::string::npos) continue;
    libView.push_back(i);
  }
}


std::string formatBytes(uint64_t b) {
  char buf[64];
  if (b >= 1000000000ULL)
    snprintf(buf, sizeof buf, "%.1f GB", b / 1e9);
  else if (b >= 1000000ULL)
    snprintf(buf, sizeof buf, "%.0f MB", b / 1e6);
  else if (b >= 1000ULL)
    snprintf(buf, sizeof buf, "%.0f KB", b / 1e3);
  else
    snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(b));
  return std::string(buf);
}

void drawTextCentered(const std::string &t, int y, SDL_Color c,
                      TTF_Font *f = nullptr) {
  if (!f) f = font;
  if (!f) return;
  int w = 0;
  TTF_SizeUTF8(f, t.c_str(), &w, nullptr);
  drawText(t, (scrW - w) / 2, y, c, f);
}

// Simple word wrap for dialog bodies (Vietnamese text has spaces).
void drawWrapped(const std::string &text, int x, int y, int maxW, SDL_Color c,
                 TTF_Font *f, int lineH) {
  size_t start = 0;
  int cy = y;
  while (start < text.size()) {
    const size_t nl = text.find('\n', start);
    const std::string para = text.substr(
        start, nl == std::string::npos ? std::string::npos : nl - start);
    size_t wpos = 0;
    bool firstLine = true;
    while (wpos < para.size() || firstLine) {
      firstLine = false;
      std::string line;
      size_t wend = wpos;
      while (wend < para.size()) {
        const size_t sp = para.find(' ', wend);
        const std::string word = para.substr(
            wend, sp == std::string::npos ? std::string::npos : sp - wend);
        const std::string cand = line.empty() ? word : line + " " + word;
        int w = 0;
        TTF_SizeUTF8(f, cand.c_str(), &w, nullptr);
        if (w > maxW && !line.empty()) break;
        line = cand;
        if (sp == std::string::npos) {
          wend = para.size();
          break;
        }
        wend = sp + 1;
      }
      if (line.empty() && wpos < para.size()) {
        line = para.substr(wpos, 24);
        wend = wpos + 24;
      }
      if (!line.empty()) drawText(line, x, cy, c, f);
      cy += lineH;
      wpos = wend;
      while (wpos < para.size() && para[wpos] == ' ') ++wpos;
      if (wpos >= para.size()) break;
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
    cy += lineH / 4;
  }
}

void drawEmptyBox(int y, int h, const char *line1, const char *line2) {
  fillRect(X(40), y, X(944), h, color(30, 36, 46));
  drawText(line1, X(68), y + Y(22), color(225, 230, 238));
  drawText(line2, X(68), y + Y(58), color(150, 160, 176), fontSmall);
}

// ---------------- virtual keyboard ----------------

void openKeyboard(const std::string &title, const std::string &initial,
                  KbMode mode) {
  vkey.open = true;
  vkey.title = title;
  vkey.buffer = initial;
  vkey.row = 0;
  vkey.col = 0;
  vkey.done = false;
  vkey.cancelled = false;
  kbMode = mode;
}

void handleKeyboardInput(const InputState &in) {
  const int rows = kKeyRowCount;
  auto rowLen = [&](int r) { return static_cast<int>(strlen(kKeyRows[r])); };
  if (in.up) {
    vkey.row = (vkey.row + rows - 1) % rows;
    vkey.col = std::min(vkey.col, rowLen(vkey.row) - 1);
  }
  if (in.down) {
    vkey.row = (vkey.row + 1) % rows;
    vkey.col = std::min(vkey.col, rowLen(vkey.row) - 1);
  }
  if (in.left) {
    const int L = rowLen(vkey.row);
    vkey.col = (vkey.col + L - 1) % L;
  }
  if (in.right) {
    const int L = rowLen(vkey.row);
    vkey.col = (vkey.col + 1) % L;
  }
  if (in.a) vkey.buffer.push_back(kKeyRows[vkey.row][vkey.col]);
  if (in.x && !vkey.buffer.empty()) vkey.buffer.pop_back();
  if (in.y || in.start) {
    vkey.done = true;
    vkey.open = false;
  }
  if (in.b) {
    vkey.cancelled = true;
    vkey.open = false;
  }
}

void drawKeyboard() {
  fillRect(0, 0, scrW, scrH, color(0, 0, 0, 150));
  const int w = X(760), h = Y(520);
  const int x = (scrW - w) / 2, y = (scrH - h) / 2;
  fillRect(x, y, w, h, color(28, 33, 42));
  fillRect(x, y, w, Y(4), color(44, 91, 150));
  drawText(vkey.title, x + X(28), y + Y(22), color(246, 248, 252));
  // text field
  const int fx = x + X(28), fw = w - X(56), fh = Y(52), fy = y + Y(64);
  fillRect(fx, fy, fw, fh, color(15, 18, 24));
  std::string shown = vkey.buffer + "_";
  int tw = 0;
  TTF_SizeUTF8(font, shown.c_str(), &tw, nullptr);
  std::string view = shown;
  while (tw > fw - X(24) && view.size() > 1) {
    view = view.substr(1);
    TTF_SizeUTF8(font, view.c_str(), &tw, nullptr);
  }
  drawText(view, fx + X(12), fy + Y(10), color(240, 244, 250), font);
  // keys
  const int keyW = X(50), keyH = Y(52), gapX = X(6), gapY = Y(8);
  const int gridY = fy + fh + Y(20);
  for (int r = 0; r < kKeyRowCount; ++r) {
    const int L = static_cast<int>(strlen(kKeyRows[r]));
    const int rowW = L * keyW + (L - 1) * gapX;
    const int rowX = x + (w - rowW) / 2;
    const int ky = gridY + r * (keyH + gapY);
    for (int c = 0; c < L; ++c) {
      const int kx = rowX + c * (keyW + gapX);
      const bool sel = (r == vkey.row && c == vkey.col);
      fillRect(kx, ky, keyW, keyH,
               sel ? color(44, 91, 150) : color(45, 52, 64));
      char label[2] = {kKeyRows[r][c], '\0'};
      int lw = 0;
      TTF_SizeUTF8(font, label, &lw, nullptr);
      drawText(label, kx + (keyW - lw) / 2, ky + Y(10), color(240, 244, 250),
               font);
    }
  }
  drawText("D-Pad Di chuyển · A Chọn ký tự · X Xóa · Y Xong · B Hủy",
           x + X(28), y + h - Y(40), color(150, 160, 176), fontSmall,
           w - X(56));
}

void onKeyboardDone() {
  if (kbMode == KbMode::Search) {
    libQuery = trimStr(vkey.buffer);
    libSearching = !libQuery.empty();
    refreshLibView();
  } else if (kbMode == KbMode::AddLink) {
    const std::string id = parseDriveFolderId(vkey.buffer);
    if (id.empty()) {
      dlg.open = true;
      dlg.title = "Link không hợp lệ";
      dlg.body =
          "Không tách được folder ID từ link.\nHãy dán link dạng:\n"
          "https://drive.google.com/drive/folders/<ID>";
    } else {
      std::ofstream f(appDirectory + "/data/DriveLinks.txt", std::ios::app);
      f << trimStr(vkey.buffer) << "\n";
      gDriveLinks = loadDriveLinks(appDirectory);
      dlg.open = true;
      dlg.title = "Đã thêm nguồn";
      dlg.body = "Folder ID: " + id + "\nDùng \"Kiểm tra kết nối\" ở màn hình Drive.";
    }
  } else if (kbMode == KbMode::AddApiKey) {
    const std::string key = trimStr(vkey.buffer);
    if (key.size() < 10) {
      dlg.open = true;
      dlg.title = "API key không hợp lệ";
      dlg.body = "Key quá ngắn. Hãy kiểm tra lại key\nđã copy từ Google Cloud Console.";
    } else {
      AppConfig::instance().setSetting("drive_api_key", key);
      dlg.open = true;
      dlg.title = "Đã lưu API key";
      dlg.body =
          "Key đã lưu trên máy này (data/drive_api_key).\nKhông bao giờ commit lên repo.";
    }
  }
  kbMode = KbMode::None;
}

// ---------------- dialog ----------------

void drawDialog() {
  fillRect(0, 0, scrW, scrH, color(0, 0, 0, 150));
  const int w = X(640), h = Y(320);
  const int x = (scrW - w) / 2, y = (scrH - h) / 2;
  fillRect(x, y, w, h, color(28, 33, 42));
  fillRect(x, y, w, Y(4), color(44, 91, 150));
  drawText(dlg.title, x + X(28), y + Y(24), color(246, 248, 252));
  drawWrapped(dlg.body, x + X(28), y + Y(70), w - X(56), color(200, 208, 220),
              fontSmall, Y(28));
  drawText("Nhấn A để đóng", x + X(28), y + h - Y(44), color(150, 160, 176),
           fontSmall);
}

void showDialog(const std::string &title, const std::string &body) {
  dlg.open = true;
  dlg.title = title;
  dlg.body = body;
}

// ---------------- donate QR (SDL2_image via dlopen, optional) ----------------

bool loadQrTexture() {
  if (qrTried) return qrOk;
  qrTried = true;
  void *h = dlopen("libSDL2_image-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
  if (!h) h = dlopen("libSDL2_image-2.0.so", RTLD_NOW | RTLD_LOCAL);
  if (!h) return false;
  using InitFn = int (*)(int);
  using LoadFn = SDL_Surface *(*)(const char *);
  auto pInit = reinterpret_cast<InitFn>(dlsym(h, "IMG_Init"));
  auto pLoad = reinterpret_cast<LoadFn>(dlsym(h, "IMG_Load"));
  if (pInit) pInit(0x2); // IMG_INIT_PNG
  if (!pLoad) return false;
  SDL_Surface *s = pLoad((appDirectory + "/assets/donate-qr.png").c_str());
  if (!s) return false;
  qrTex = SDL_CreateTextureFromSurface(renderer, s);
  qrW = s->w;
  qrH = s->h;
  SDL_FreeSurface(s);
  qrOk = (qrTex != nullptr);
  return qrOk;
}

void drawHeader() {
  fillRect(0, 0, scrW, headerH(), color(22, 27, 35));
  drawText("Trimui-GameSync", X(28), Y(17), color(246, 248, 252));
  const std::string ver = "v" + appVersion;
  int width = 0;
  if (TTF_SizeUTF8(fontSmall, ver.c_str(), &width, nullptr) == 0) {
    drawText(ver, scrW - width - X(28), Y(22), color(145, 190, 255), fontSmall);
  }
}

// ---------------- screens ----------------

void drawHome() {
  drawText("Quản lý game trên máy + đồng bộ Google Drive", X(40), Y(88),
           color(150, 160, 176), fontSmall);
  // info cards 2x2
  const int cw = X(452), ch = Y(100), gap = X(40);
  const int cx[2] = {X(40), X(40) + cw + gap};
  const int cy[2] = {Y(130), Y(130) + ch + Y(16)};
  const char *labels[4] = {"Game trên máy", "Hệ máy", "Dung lượng Roms",
                           "Drive"};
  std::string values[4];
  values[0] = scanDone ? std::to_string(scanFileCount.load()) : "—";
  values[1] = std::to_string(gSystems.size());
  if (scanRunning)
    values[2] = "Đang quét… " + std::to_string(scanProgress.load());
  else
    values[2] = scanDone ? formatBytes(scanTotalBytes.load()) : "—";
  values[3] = gDriveLinks.empty()
                  ? "Chưa có link"
                  : "Đã cấu hình (" + std::to_string(gDriveLinks.size()) + ")";
  for (int i = 0; i < 4; ++i) {
    const int x = cx[i % 2], y = cy[i / 2];
    fillRect(x, y, cw, ch, color(30, 36, 46));
    fillRect(x, y, X(5), ch, color(44, 91, 150));
    drawText(labels[i], x + X(24), y + Y(14), color(150, 160, 176), fontSmall);
    drawText(values[i], x + X(24), y + Y(46), color(246, 248, 252), font);
  }
  // quick actions
  int y = Y(380);
  drawText("Thao tác nhanh", X(40), y, color(225, 230, 238), fontSmall);
  y += Y(34);
  for (int i = 0; i < kHomeActionCount; ++i) {
    const bool sel = i == homeSel;
    fillRect(X(40), y, X(944), Y(64),
             sel ? color(44, 91, 150) : color(30, 36, 46));
    drawText(kHomeActions[i].title, X(68), y + Y(8), color(247, 249, 252),
             font);
    drawText(kHomeActions[i].subtitle, X(68), y + Y(36), color(177, 189, 206),
             fontSmall);
    y += Y(72);
  }
  drawFooter("D-Pad Chọn · A Mở · Y Đồng bộ Drive", "B Thoát");
}

void drawSysTabs() {
  const int y = Y(88), h = Y(52);
  const int tabW = X(96), gap = X(6);
  const int viewX = X(40), viewW = X(944);
  const int selX = libTab * (tabW + gap);
  if (selX < libTabScroll) libTabScroll = selX;
  if (selX + tabW > libTabScroll + viewW) libTabScroll = selX + tabW - viewW;
  if (libTabScroll < 0) libTabScroll = 0;
  for (size_t i = 0; i < libTabs.size(); ++i) {
    const int tx = viewX + static_cast<int>(i) * (tabW + gap) - libTabScroll;
    if (tx + tabW < viewX || tx > viewX + viewW) continue;
    const bool sel = static_cast<int>(i) == libTab;
    fillRect(tx, y, tabW, h, sel ? color(44, 91, 150) : color(30, 36, 46));
    drawText(libTabs[i].label, tx + X(10), y + Y(13),
             sel ? color(255, 255, 255) : color(177, 189, 206), fontSmall,
             tabW - X(20));
  }
}

void drawLibrary() {
  if (scanRunning) {
    char buf[128];
    snprintf(buf, sizeof buf, "Đang quét thư viện… %llu file",
             static_cast<unsigned long long>(scanProgress.load()));
    drawEmptyBox(Y(200), Y(140), buf, "Vui lòng chờ, UI vẫn dùng được.");
    drawFooter("", "B Về Home");
    return;
  }
  if (!scanDone) {
    drawEmptyBox(Y(200), Y(140), "Chưa có dữ liệu thư viện",
                 "Nhấn Y để quét game trên máy.");
    drawFooter("Y Quét thư viện", "B Về Home");
    return;
  }
  if (libTabs.empty()) {
    drawEmptyBox(Y(200), Y(140), "Không tìm thấy game nào",
                 "Thư mục Roms trống hoặc không đọc được.");
    drawFooter("Y Quét lại", "B Về Home");
    return;
  }
  drawSysTabs();
  const LibTab &t = libTabs[libTab];
  char sub[256];
  if (t.code.empty()) {
    snprintf(sub, sizeof sub, "Khác  →  %d game không thuộc hệ máy đã biết",
             t.count);
  } else {
    snprintf(sub, sizeof sub, "%s  →  Roms/%s   ·   %d game%s",
             t.driveName.c_str(), t.code.c_str(), t.count,
             t.unsupported ? "   (máy không hỗ trợ — vẫn lưu trữ được)" : "");
  }
  drawText(sub, X(40), Y(152), color(150, 160, 176), fontSmall, X(944));
  const int listTop = libSearching ? Y(222) : Y(196);
  if (libSearching) {
    drawText(("Tìm kiếm: \"" + libQuery + "\"").c_str(), X(40), Y(186),
             color(242, 200, 120), fontSmall, X(944));
  }
  const int rowH = Y(56), viewY = listTop, viewH = Y(430);
  const int selY = libSel * rowH;
  if (selY < libScroll) libScroll = selY;
  if (selY + rowH > libScroll + viewH) libScroll = selY + rowH - viewH;
  if (libScroll < 0) libScroll = 0;
  int y = viewY - libScroll;
  {
    std::lock_guard<std::mutex> lk(gScanMutex);
    for (size_t vi = 0; vi < libView.size(); ++vi) {
      if (y + rowH >= viewY && y <= viewY + viewH) {
        const GameFile &f = gScan.files[libView[vi]];
        const bool sel = static_cast<int>(vi) == libSel;
        fillRect(X(40), y, X(944), rowH,
                 sel ? color(44, 91, 150) : color(30, 36, 46));
        drawText(f.name, X(68), y + Y(13),
                 sel ? color(255, 255, 255) : color(225, 230, 238), font,
                 X(700));
        const std::string sz = formatBytes(f.size);
        int w = 0;
        TTF_SizeUTF8(fontSmall, sz.c_str(), &w, nullptr);
        drawText(sz, X(40) + X(944) - w - X(28), y + Y(16),
                 color(150, 160, 176), fontSmall);
      }
      y += rowH;
    }
  }
  if (libView.empty()) {
    drawEmptyBox(viewY, Y(120), "Không tìm thấy",
                 libSearching ? "Thử từ khóa khác (gõ không dấu vẫn được)."
                              : "Hệ máy này chưa có game.");
  }
  drawFooter("◀▶ Đổi hệ · ▲▼ Chọn · X Tìm kiếm · Y Quét lại", "B Về Home");
}

void drawDrive() {
  drawText("Nguồn game trên Google Drive (link do bạn thêm)", X(40), Y(88),
           color(150, 160, 176), fontSmall);
  const int rowH = Y(96), actH = Y(64);
  const int total = static_cast<int>(gDriveLinks.size()) + 3;
  const int viewY = Y(130), viewH = Y(470);
  const int selY = driveSel * (rowH + Y(10));
  if (selY < driveScroll) driveScroll = selY;
  if (selY + rowH > driveScroll + viewH) driveScroll = selY + rowH - viewH;
  if (driveScroll < 0) driveScroll = 0;
  int y = viewY - driveScroll;
  for (size_t i = 0; i < gDriveLinks.size(); ++i) {
    if (y + rowH >= viewY && y <= viewY + viewH) {
      const bool sel = static_cast<int>(i) == driveSel;
      fillRect(X(40), y, X(944), rowH,
               sel ? color(44, 91, 150) : color(30, 36, 46));
      char t[64];
      snprintf(t, sizeof t, "Nguồn %zu", i + 1);
      drawText(t, X(68), y + Y(10), color(247, 249, 252), font);
      drawText(shortMiddle(gDriveLinks[i]).c_str(), X(68), y + Y(42),
               color(145, 190, 255), fontSmall, X(888));
      const std::string fid = parseDriveFolderId(gDriveLinks[i]);
      const uint64_t cached = fid.empty() ? 0 : driveCachedCount(fid);
      char m[128];
      snprintf(m, sizeof m, "Cache: %llu file · Map %zu hệ máy ↔ Roms/",
               (unsigned long long)cached, gSystems.size());
      drawText(m, X(68), y + Y(66), color(150, 160, 176), fontSmall, X(888));
    }
    y += rowH + Y(10);
  }
  const char *acts[3] = {"+ Thêm link Drive", "» Đồng bộ Drive",
                           "? Kiểm tra kết nối"};
  for (int a = 0; a < 3; ++a) {
    const int idx = static_cast<int>(gDriveLinks.size()) + a;
    if (y + actH >= viewY && y <= viewY + viewH) {
      const bool sel = idx == driveSel;
      fillRect(X(40), y, X(944), actH,
               sel ? color(44, 91, 150) : color(30, 36, 46));
      drawText(acts[a], X(68), y + Y(16), color(247, 249, 252), font);
    }
    y += actH + Y(10);
  }
  if (driveNetRunning) {
    char p[96];
    const char *spin = "|/-\\";
    snprintf(p, sizeof p, "Đang xử lý Drive… %llu file %c",
             (unsigned long long)driveNetProgress.load(),
             spin[(SDL_GetTicks() / 250) % 4]);
    drawText(p, X(40), Y(620), color(242, 200, 120), fontSmall);
  } else if (gDriveLinks.empty()) {
    drawText("Chưa có nguồn nào — chọn \"Thêm link Drive\" bên dưới.", X(40),
             Y(620), color(150, 160, 176), fontSmall);
  }
  (void)total;
  if (driveNetRunning)
    drawFooter("D-Pad Chọn · A Thực hiện · Y Quét Drive", "B Hủy quét");
  else
    drawFooter("D-Pad Chọn · A Thực hiện · Y Quét Drive", "B Về Home");
}

void showConfirm(const std::string &title, const std::string &body,
                 std::function<void()> onYes) {
  cfm.open = true;
  cfm.title = title;
  cfm.body = body;
  cfm.onYes = std::move(onYes);
}

void drawConfirm() {
  fillRect(0, 0, scrW, scrH, color(0, 0, 0, 150));
  const int w = X(680), h = Y(360);
  const int x = (scrW - w) / 2, y = (scrH - h) / 2;
  fillRect(x, y, w, h, color(28, 33, 42));
  fillRect(x, y, w, Y(4), color(44, 91, 150));
  drawText(cfm.title, x + X(28), y + Y(24), color(246, 248, 252));
  drawWrapped(cfm.body, x + X(28), y + Y(70), w - X(56), color(200, 208, 220),
              fontSmall, Y(28));
  drawText("A Đồng ý", x + X(28), y + h - Y(44), color(120, 200, 130),
           fontSmall);
  drawText("B Hủy", x + X(220), y + h - Y(44), color(150, 160, 176), fontSmall);
}

std::string syncRowKey(const DiffEntry &e) {
  return e.system + "/" + e.relPath;
}

void recomputeDiff() {
  std::vector<DriveFile> remote;
  for (const auto &link : gDriveLinks) {
    const std::string id = parseDriveFolderId(link);
    if (id.empty()) continue;
    std::vector<DriveFile> files;
    if (loadDriveCache(driveCachePath(appDirectory + "/data", id), files))
      remote.insert(remote.end(), files.begin(), files.end());
  }
  std::vector<DiffEntry> diff;
  {
    std::lock_guard<std::mutex> lk(gScanMutex);
    diff = compareLibrary(gScan, remote);
  }
  {
    std::lock_guard<std::mutex> lk(gDiffMutex);
    gDiff = std::move(diff);
  }
  syncDirty = false;
  syncSel = 0;
  syncScroll = 0;
  syncMarked.clear();
}

// Indices into gDiff for the active filter tab.
std::vector<int> buildSyncView() {
  std::vector<int> out;
  std::lock_guard<std::mutex> lk(gDiffMutex);
  for (size_t i = 0; i < gDiff.size(); ++i) {
    const DiffStatus st = gDiff[i].status;
    if (syncTab == 1 && st != DiffStatus::REMOTE_ONLY) continue;
    if (syncTab == 2 && st != DiffStatus::LOCAL_ONLY) continue;
    if (syncTab == 3 && st != DiffStatus::DIFFER) continue;
    out.push_back(static_cast<int>(i));
  }
  return out;
}

SDL_Color statusColor(DiffStatus st) {
  switch (st) {
    case DiffStatus::SYNCED: return color(120, 200, 130);
    case DiffStatus::REMOTE_ONLY: return color(145, 190, 255);
    case DiffStatus::LOCAL_ONLY: return color(150, 160, 176);
    case DiffStatus::DIFFER: return color(242, 200, 120);
  }
  return color(200, 200, 200);
}

// Chuỗi "Đồng bộ tất cả" (nút Start ở màn hình Sync):
// (a) quét game trên máy nếu chưa có, (b) quét Drive, (c) so sánh + báo kết quả.
// Bước (b)/(c) được nối qua pendingFullSync trong main loop.
void startFullSync() {
  if (syncMode == SyncMode::Queue) return;  // đang ở màn hình tải
  if (scanRunning || driveNetRunning) {
    showDialog("Đang xử lý", "Vui lòng chờ tác vụ hiện tại xong.");
    return;
  }
  if (resolveDriveApiKey().empty()) {
    showDialog("Thiếu API key",
               "Vào Cài đặt → Nhập API key Drive\n(hoặc đặt env GS_DRIVE_API_KEY).");
    return;
  }
  if (gDriveLinks.empty()) {
    showDialog("Chưa có nguồn", "Thêm link Drive trước đã.");
    return;
  }
  pendingFullSync = true;
  if (!scanDone) {
    startScan();  // async → scanFinishedFlag → nối tiếp drive rescan
  } else {
    startDriveRescan();
    if (!driveNetRunning) pendingFullSync = false;  // start thất bại
  }
}

// Tải ngay 1 dòng (bấm A) kiểu RomCloud: không cần đánh dấu X trước.
void downloadSingleEntry(const DiffEntry &e) {
  if (e.status != DiffStatus::REMOTE_ONLY && e.status != DiffStatus::DIFFER)
    return;  // chỉ tải được game trên Drive
  const std::string key = resolveDriveApiKey();
  if (key.empty()) {
    showDialog("Thiếu API key",
               "Vào Cài đặt → Nhập API key Drive\n(hoặc đặt env GS_DRIVE_API_KEY).");
    return;
  }
  int skipped = 0;
  const std::string romsDir = defaultRomsDir();
  const std::vector<DownloadTask> tasks =
      buildDownloadTasks(std::vector<DiffEntry>{e}, romsDir, skipped);
  if (tasks.empty()) {
    showDialog("Không tải được", "File này không tải được (tên file không an toàn?).");
    return;
  }
  uint64_t total = 0;
  for (const auto &t : tasks) total += t.size;
  gQueue.reset(new DownloadQueue(key, appDirectory, romsDir));
  gQueue->enqueue(tasks);
  uint64_t need = 0, freeB = 0;
  if (!gQueue->preflight(need, freeB)) {
    char b[512];
    snprintf(b, sizeof b,
             "Cần %s (đã gồm 200MB dự phòng)\nTrống: %s\nBỏ bớt file rồi thử lại.",
             formatBytes(need).c_str(), formatBytes(freeB).c_str());
    showDialog("Không đủ dung lượng", b);
    gQueue.reset();
    return;
  }
  char b[768];
  snprintf(b, sizeof b, "Tải \"%s\" (%s)\nvề Roms/%s\n\nA: Tải    B: Hủy",
           e.name.c_str(), formatBytes(total).c_str(),
           e.system.empty() ? "?" : e.system.c_str());
  showConfirm("Tải game", b, [] {
    queueAuthShown = false;
    queueDoneHandled = false;
    syncMode = SyncMode::Queue;
    syncScroll = 0;
    gQueue->start();
    Logger::info("Single download started");
  });
}

void startSyncFlow() {
  std::vector<DiffEntry> marked;
  {
    std::lock_guard<std::mutex> lk(gDiffMutex);
    for (const auto &e : gDiff)
      if (syncMarked.count(syncRowKey(e)) > 0) marked.push_back(e);
  }
  if (marked.empty()) {
    showDialog("Đồng bộ", "Chưa đánh dấu dòng nào.\nNhấn X để đánh dấu các "
                          "game cần tải về.");
    return;
  }
  const std::string key = resolveDriveApiKey();
  if (key.empty()) {
    showDialog("Thiếu API key", "Vào Cài đặt → Nhập API key Drive\n(hoặc đặt "
                                "env GS_DRIVE_API_KEY).");
    return;
  }
  int skipped = 0;
  const std::string romsDir = defaultRomsDir();
  const std::vector<DownloadTask> tasks =
      buildDownloadTasks(marked, romsDir, skipped);
  if (tasks.empty()) {
    showDialog("Đồng bộ", "Các dòng đã đánh dấu không có gì để tải.\n(Chỉ tải "
                          "được: ↓ Máy thiếu, ≠ Khác bản)");
    return;
  }
  uint64_t total = 0;
  for (const auto &t : tasks) total += t.size;
  gQueue.reset(new DownloadQueue(key, appDirectory, romsDir));
  gQueue->enqueue(tasks);
  uint64_t need = 0, freeB = 0;
  if (!gQueue->preflight(need, freeB)) {
    char b[512];
    snprintf(b, sizeof b,
             "Cần %s (đã gồm 200MB dự phòng)\nTrống: %s\nBỏ bớt file rồi "
             "thử lại.",
             formatBytes(need).c_str(), formatBytes(freeB).c_str());
    showDialog("Không đủ dung lượng", b);
    gQueue.reset();
    return;
  }
  char b[768];
  snprintf(b, sizeof b,
           "Tải %zu file (%s) về Roms/\nDung lượng trống: %s%s\n\nA: Bắt "
           "đầu    B: Hủy",
           tasks.size(), formatBytes(total).c_str(),
           formatBytes(freeB).c_str(),
           skipped > 0
               ? ("\nBỏ qua " + std::to_string(skipped) +
                  " file tên không an toàn")
                     .c_str()
               : "");
  showConfirm("Xác nhận đồng bộ", b, [] {
    queueAuthShown = false;
    queueDoneHandled = false;
    syncMode = SyncMode::Queue;
    syncScroll = 0;
    gQueue->start();
    Logger::info("Sync queue started");
  });
}
void drawSyncQueue() {
  const std::vector<TaskView> views =
      gQueue ? gQueue->snapshot() : std::vector<TaskView>();
  const uint64_t total = gQueue ? gQueue->totalBytes() : 0;
  const uint64_t done = gQueue ? gQueue->doneBytes() : 0;
  int doneN = 0;
  for (const auto &v : views)
    if (v.state == TaskState::Done) ++doneN;
  const int pct = total ? static_cast<int>(done * 100 / total) : 0;
  char head[192];
  snprintf(head, sizeof head, "Hàng đợi tải  ·  %d/%zu file xong  ·  %s / %s "
                              "(%d%%)%s",
           doneN, views.size(), formatBytes(done).c_str(),
           formatBytes(total).c_str(), pct,
           (gQueue && gQueue->paused()) ? "  ·  TẠM DỪNG" : "");
  drawText(head, X(40), Y(88), color(225, 230, 238), fontSmall, X(944));
  fillRect(X(40), Y(122), X(944), Y(14), color(30, 36, 46));
  fillRect(X(40), Y(122), X(944) * pct / 100, Y(14), color(44, 91, 150));
  const int rowH = Y(84), viewY = Y(150), viewH = Y(460);
  int y = viewY - syncScroll;
  for (size_t i = 0; i < views.size(); ++i) {
    const TaskView &v = views[i];
    if (y + rowH >= viewY && y <= viewY + viewH) {
      fillRect(X(40), y, X(944), rowH, color(30, 36, 46));
      const char *st = "";
      SDL_Color sc = color(150, 160, 176);
      switch (v.state) {
        case TaskState::Queued: st = "Chờ"; break;
        case TaskState::Downloading:
          st = "Đang tải";
          sc = color(145, 190, 255);
          break;
        case TaskState::Paused:
          st = "Tạm dừng";
          sc = color(242, 200, 120);
          break;
        case TaskState::Done:
          st = "Xong";
          sc = color(120, 200, 130);
          break;
        case TaskState::Error:
          st = "Lỗi";
          sc = color(230, 110, 110);
          break;
        case TaskState::Cancelled: st = "Đã hủy"; break;
      }
      drawText(v.label, X(68), y + Y(8), color(225, 230, 238), fontSmall,
               X(700));
      drawText(st, X(790), y + Y(8), sc, fontSmall);
      const int bx = X(68), bw = X(700);
      fillRect(bx, y + Y(38), bw, Y(10), color(20, 24, 32));
      const int p = v.size ? static_cast<int>(v.done * 100 / v.size) : 0;
      fillRect(bx, y + Y(38), bw * p / 100, Y(10),
               v.state == TaskState::Error ? color(180, 70, 70)
                                           : color(44, 91, 150));
      char sub[192];
      if (v.state == TaskState::Error) {
        snprintf(sub, sizeof sub, "%s", v.error.c_str());
        drawText(sub, X(68), y + Y(56), color(230, 130, 130), fontSmall,
                 X(876));
      } else {
        char eta[32] = "--";
        if (v.speed > 1 && v.size > v.done) {
          const int sec = static_cast<int>((v.size - v.done) / v.speed);
          snprintf(eta, sizeof eta, "%02d:%02d", sec / 60, sec % 60);
        }
        snprintf(sub, sizeof sub, "%s / %s  ·  %s/s  ·  còn %s",
                 formatBytes(v.done).c_str(), formatBytes(v.size).c_str(),
                 formatBytes(static_cast<uint64_t>(v.speed)).c_str(), eta);
        drawText(sub, X(68), y + Y(56), color(150, 160, 176), fontSmall,
                 X(876));
      }
    }
    y += rowH + Y(6);
  }
  const bool fin = gQueue && gQueue->finished();
  drawFooter(fin ? "B Về So sánh" : "A Tạm dừng/Tiep tuc · B Dung hang doi",
             "Đồng bộ Drive → máy");
}

void drawSync() {
  if (syncMode == SyncMode::Queue) {
    drawSyncQueue();
    return;
  }
  if (syncDirty && !scanRunning && !driveNetRunning) recomputeDiff();
  int tx = X(40);
  for (int i = 0; i < kSyncTabCount; ++i) {
    int tw = 0;
    TTF_SizeUTF8(fontSmall, kSyncTabs[i], &tw, nullptr);
    tw += X(36);
    const bool sel = i == syncTab;
    fillRect(tx, Y(88), tw, Y(48), sel ? color(44, 91, 150) : color(30, 36, 46));
    drawText(kSyncTabs[i], tx + X(18), Y(101),
             sel ? color(255, 255, 255) : color(177, 189, 206), fontSmall);
    tx += tw + X(10);
  }
  const std::vector<int> view = buildSyncView();
  const int rowH = Y(56), viewY = Y(160), viewH = Y(440);
  const int selY = syncSel * rowH;
  if (selY < syncScroll) syncScroll = selY;
  if (selY + rowH > syncScroll + viewH) syncScroll = selY + rowH - viewH;
  if (syncScroll < 0) syncScroll = 0;
  int y = viewY - syncScroll;
  {
    std::lock_guard<std::mutex> lk(gDiffMutex);
    for (size_t vi = 0; vi < view.size(); ++vi) {
      const DiffEntry &e = gDiff[view[vi]];
      if (y + rowH >= viewY && y <= viewY + viewH) {
        const bool sel = static_cast<int>(vi) == syncSel;
        const bool marked = syncMarked.count(syncRowKey(e)) > 0;
        fillRect(X(40), y, X(944), rowH,
                 sel ? color(44, 91, 150) : color(30, 36, 46));
        drawText(marked ? "[x]" : "[ ]", X(56), y + Y(15),
                 marked ? color(242, 200, 120) : color(120, 130, 145),
                 fontSmall);
        std::string nm = e.name;
        if (e.isGroup)
          nm += "  [" + std::to_string(e.members.size()) + " file]";
        drawText(nm, X(112), y + Y(6),
                 sel ? color(255, 255, 255) : color(225, 230, 238), font,
                 X(430));
        drawText(e.system.empty() ? "Khác" : e.system, X(112), y + Y(33),
                 color(150, 160, 176), fontSmall);
        {
          // Badge mau trang thai (hoc RomCloud): cham tron mau + nhan chu
          SDL_Color sc = statusColor(e.status);
          fillRect(X(600), y + Y(20), X(16), X(16), sc);
          drawText(diffStatusLabel(e.status), X(624), y + Y(15), sc, fontSmall);
        }
        const uint64_t sz = (e.status == DiffStatus::LOCAL_ONLY ||
                             e.status == DiffStatus::SYNCED)
                                ? e.localSize
                                : e.remoteSize;
        const std::string szs = formatBytes(sz);
        int w = 0;
        TTF_SizeUTF8(fontSmall, szs.c_str(), &w, nullptr);
        drawText(szs, X(40) + X(944) - w - X(28), y + Y(15),
                 color(150, 160, 176), fontSmall);
      }
      y += rowH;
    }
  }
  if (view.empty()) {
    const char *hint = !scanDone ? "Bấm Start để đồng bộ tất cả."
                       : gDriveLinks.empty()
                           ? "Thêm link Drive rồi bấm Start."
                           : "Bấm Start để quét Drive và so sánh.";
    drawEmptyBox(viewY, Y(120), "Không có dòng nào", hint);
  }
  if (scanRunning || driveNetRunning) {
    char p[160];
    const char *spin = "|/-\\";
    const char sc = spin[(SDL_GetTicks() / 250) % 4];
    if (scanRunning)
      snprintf(p, sizeof p, "Đang quét game trên máy… %llu file %c",
               (unsigned long long)scanProgress.load(), sc);
    else
      snprintf(p, sizeof p, "Đang lấy game trên Drive… %llu file %c",
               (unsigned long long)driveNetProgress.load(), sc);
    drawText(p, X(40), Y(620), color(242, 200, 120), fontSmall);
  } else {
    uint64_t dlBytes = 0;
    {
      std::lock_guard<std::mutex> lk(gDiffMutex);
      for (const auto &e : gDiff) {
        if (syncMarked.count(syncRowKey(e)) == 0) continue;
        if (e.status == DiffStatus::REMOTE_ONLY ||
            e.status == DiffStatus::DIFFER)
          dlBytes += e.remoteSize;
      }
    }
    char buf[128];
    snprintf(buf, sizeof buf, "Đã đánh dấu: %zu dòng · sẽ tải %s",
             syncMarked.size(), formatBytes(dlBytes).c_str());
    drawText(buf, X(40), Y(620), color(150, 160, 176), fontSmall);
  }
  drawFooter("A Tải ngay · X Đánh dấu · Y Tải đã đánh dấu · Start Đồng bộ tất cả",
             "◀▶ Lọc · ▲▼ Chọn · B Về Home");
}

void drawFix() {
  if (fixDirty && !scanRunning) {
    rebuildFix();
    fixDirty = false;
  }
  drawText("Phát hiện file game đặt sai thư mục Roms/ và di chuyển về đúng "
           "chỗ.",
           X(40), Y(88), color(150, 160, 176), fontSmall, X(944));
  int nMove = 0, nWarn = 0;
  {
    std::lock_guard<std::mutex> lk(gFixMutex);
    for (const auto &e : gFix)
      if (e.kind == FixKind::Move)
        ++nMove;
      else
        ++nWarn;
  }
  char cnt[192];
  snprintf(cnt, sizeof cnt,
           "Đề xuất di chuyển: %d · Cần xem tay: %d · Đã đánh dấu: %zu", nMove,
           nWarn, fixMarked.size());
  drawText(cnt, X(40), Y(120), color(225, 230, 238), fontSmall);

  const int rowH = Y(56), viewY = Y(160), viewH = Y(436);
  const int selY = fixSel * rowH;
  if (selY < fixScroll) fixScroll = selY;
  if (selY + rowH > fixScroll + viewH) fixScroll = selY + rowH - viewH;
  if (fixScroll < 0) fixScroll = 0;
  int y = viewY - fixScroll;
  {
    std::lock_guard<std::mutex> lk(gFixMutex);
    for (size_t vi = 0; vi < gFix.size(); ++vi) {
      const FixEntry &e = gFix[vi];
      if (y + rowH >= viewY && y <= viewY + viewH) {
        const bool sel = static_cast<int>(vi) == fixSel;
        const bool marked = fixMarked.count(fixRowKey(e)) > 0;
        const bool canMove = e.kind == FixKind::Move;
        fillRect(X(40), y, X(944), rowH,
                 sel ? color(44, 91, 150) : color(30, 36, 46));
        drawText(canMove ? (marked ? "[x]" : "[ ]") : " ! ", X(56), y + Y(15),
                 canMove ? (marked ? color(242, 200, 120)
                                  : color(120, 130, 145))
                         : color(242, 200, 120),
                 fontSmall);
        drawText(e.name, X(112), y + Y(6),
                 sel ? color(255, 255, 255) : color(225, 230, 238), font,
                 X(560));
        if (canMove) {
          char dst[128];
          snprintf(dst, sizeof dst, "%s  →  %s",
                   e.system.empty() ? "(gốc Roms)" : e.system.c_str(),
                   e.expected.c_str());
          drawText(dst, X(112), y + Y(33), color(242, 200, 120), fontSmall,
                   X(560));
        } else {
          drawText(e.note, X(112), y + Y(33), color(150, 160, 176), fontSmall,
                   X(560));
        }
        std::string right =
            e.isGroup ? (std::to_string(e.members.size()) + " file")
                      : formatBytes(e.size);
        int w = 0;
        TTF_SizeUTF8(fontSmall, right.c_str(), &w, nullptr);
        drawText(right, X(40) + X(944) - w - X(28), y + Y(15),
                 color(150, 160, 176), fontSmall);
      }
      y += rowH;
    }
  }
  if (gFix.empty()) {
    const char *hint = !scanDone ? "Nhấn Start để quét thư viện trên máy."
                                 : "Mọi file game đã nằm đúng thư mục.";
    drawEmptyBox(viewY, Y(120), "Không phát hiện file sai thư mục", hint);
  }
  drawFooter("▲▼ Chọn · X Đánh dấu · A Di chuyển 1 · Y Di chuyển đã chọn · "
             "Start Quét",
             "B Về Home");
}

bool otaEnabled() { return !fileExists(appDirectory + "/data/.no-ota"); }

void drawSettings() {
  drawText("Cài đặt ứng dụng", X(40), Y(88), color(150, 160, 176), fontSmall);
  const char *titles[5] = {"Tự động cập nhật (OTA)", "Intro NLK khi mở app",
                           "Kết nối WiFi", "Nhập API key Drive",
                           "Xóa cache Drive"};
  std::string values[5];
  values[0] = otaEnabled() ? "Bật" : "Tắt";
  values[1] = introDisabled(appDirectory) ? "Tắt" : "Bật";
  values[2] = "Mở app WiFi của máy";
  const std::string curKey = resolveDriveApiKey();
  values[3] = curKey.empty() ? "Chưa nhập" : maskApiKey(curKey);
  values[4] = "";
  int y = Y(140);
  for (int i = 0; i < 5; ++i) {
    const bool sel = i == settingsSel;
    fillRect(X(40), y, X(944), Y(68),
             sel ? color(44, 91, 150) : color(30, 36, 46));
    drawText(titles[i], X(68), y + Y(18), color(247, 249, 252), font);
    if (!values[i].empty())
      drawText(values[i], X(880), y + Y(18), color(242, 200, 120), fontSmall);
    y += Y(78);
  }
  drawText("Ghi chú: OTA kiểm tra 30 giây sau khi mở app, rồi mỗi 10 phút.",
           X(40), Y(480), color(150, 160, 176), fontSmall, X(944));
  drawText("Tắt OTA ở đây chỉ tắt trên máy này (file data/.no-ota).", X(40),
           Y(512), color(150, 160, 176), fontSmall, X(944));
  drawFooter("D-Pad Chọn · A Đổi / Thực hiện", "B Về Home");
}

void drawAbout() {
  drawTextCentered("Trimui-GameSync", Y(110), color(246, 248, 252), font);
  char ver[64];
  snprintf(ver, sizeof ver, "Phiên bản v%s", appVersion.c_str());
  drawTextCentered(ver, Y(160), color(145, 190, 255), fontSmall);
  drawTextCentered("Đọc từ file VERSION lúc chạy — OTA đổi version",
                   Y(192), color(150, 160, 176), fontSmall);
  drawTextCentered("không cần build lại.", Y(218), color(150, 160, 176),
                   fontSmall);
  drawTextCentered("Phát triển bởi NLK", Y(280), color(225, 230, 238), font);
  drawTextCentered("Intro NLK phát mỗi lần mở app (bấm phím bất kỳ để bỏ qua).",
                   Y(326), color(150, 160, 176), fontSmall);
  const bool sel = aboutSel == 0;
  const int bw = X(320), bh = Y(72);
  const int bx = (scrW - bw) / 2, by = Y(400);
  fillRect(bx, by, bw, bh, sel ? color(44, 91, 150) : color(30, 36, 46));
  drawTextCentered("♥  Donate", by + Y(20), color(247, 249, 252), font);
  drawFooter("A Donate", "B Về Home");
}

void drawDonate() {
  drawText("Ủng hộ tác giả", X(40), Y(88), color(225, 230, 238));
  if (loadQrTexture() && qrTex && qrW > 0 && qrH > 0) {
    const int targetH = Y(360);
    const int w = static_cast<int>(qrW * (targetH / static_cast<float>(qrH)));
    const SDL_Rect dst{(scrW - w) / 2, Y(140), w, targetH};
    // white backing so the QR scans well on dark theme
    fillRect(dst.x - X(12), dst.y - Y(12), dst.w + X(24), dst.h + Y(24),
             color(255, 255, 255));
    SDL_RenderCopy(renderer, qrTex, nullptr, &dst);
    drawTextCentered("Cảm ơn bạn đã ủng hộ!", Y(540), color(246, 248, 252),
                     font);
    drawTextCentered("Mọi đóng góp giúp NLK duy trì các app TrimUI.", Y(576),
                     color(150, 160, 176), fontSmall);
  } else {
    drawEmptyBox(Y(200), Y(160), "Không tải được ảnh QR",
                 "Thiếu assets/donate-qr.png hoặc máy thiếu lib SDL2_image.");
    drawTextCentered("Cảm ơn bạn đã ủng hộ NLK!", Y(420),
                     color(246, 248, 252), font);
  }
  drawFooter("", "B Quay lại");
}

// ---------------- P2: scan control ----------------

void startScan() {
  if (scanRunning) {
    showDialog("Đang quét", "Vui lòng chờ quét hiện tại hoàn tất.");
    return;
  }
  scanRunning = true;
  scanProgress = 0;
  gScanCancel = false;
  std::thread([] {
    ScanResult res;
    scanRoms(defaultRomsDir(), res, scanProgress, gScanCancel);
    {
      std::lock_guard<std::mutex> lk(gScanMutex);
      gScan = std::move(res);
      scanDone = true;
      scanFileCount = gScan.files.size();
      uint64_t tb = 0;
      for (const auto &f : gScan.files) tb += f.size;
      scanTotalBytes = tb;
    }
    saveScanCache(appDirectory + "/data/scan_cache.bin", gScan);
    scanFinishedFlag = true;
    scanRunning = false;
    Logger::info("Scan complete: " + std::to_string(scanFileCount.load()) +
                 " files, " + std::to_string(scanTotalBytes.load()) + " bytes");
  }).detach();
}

// ---------------- P5: fix helpers (after startScan/showDialog) ----------------

// Rebuild the misplaced list from the latest scan. Keeps marks for rows
// that still exist.
void rebuildFix() {
  std::vector<FixEntry> v;
  {
    std::lock_guard<std::mutex> lk(gScanMutex);
    v = findMisplaced(gScan);
  }
  std::lock_guard<std::mutex> lk(gFixMutex);
  gFix = std::move(v);
  std::set<std::string> keep;
  for (const auto &e : gFix) {
    const std::string k = fixRowKey(e);
    if (e.kind == FixKind::Move && fixMarked.count(k) > 0) keep.insert(k);
  }
  fixMarked.swap(keep);
  if (fixSel >= static_cast<int>(gFix.size()))
    fixSel = static_cast<int>(gFix.size()) - 1;
  if (fixSel < 0) fixSel = 0;
}

// Apply move entries (already confirmed by dialog), then refresh the scan.
// Applied rows are removed from the list immediately so they don't linger
// until the background rescan finishes.
void doFixApply(std::vector<FixEntry> entries) {
  std::set<std::string> applied;
  for (const auto &e : entries) applied.insert(fixRowKey(e));
  const FixResult r = applyFixes(entries, defaultRomsDir(), appDirectory);
  {
    std::lock_guard<std::mutex> lk(gFixMutex);
    std::vector<FixEntry> keep;
    for (const auto &e : gFix)
      if (applied.count(fixRowKey(e)) == 0) keep.push_back(e);
    gFix.swap(keep);
    for (const auto &k : applied) fixMarked.erase(k);
    if (fixSel >= static_cast<int>(gFix.size()))
      fixSel = static_cast<int>(gFix.size()) - 1;
    if (fixSel < 0) fixSel = 0;
  }
  startScan();  // background refresh of gScan; fixDirty set on completion
  char b[256];
  snprintf(b, sizeof b, "Đã di chuyển %d file%s.", r.moved,
           r.skipped ? ((" (bỏ qua " + std::to_string(r.skipped) + ")").c_str())
                     : "");
  std::string msg = b;
  for (size_t i = 0; i < r.errors.size() && i < 3; ++i) msg += "\n" + r.errors[i];
  if (r.errors.size() > 3) msg += "\n…";
  msg += "\nMọi lần move đã ghi vào data/fix.log.\nNhấn A để đóng.";
  showDialog("Sửa sai thư mục", msg);
}

// ---------------- P3: Drive network ops (background threads) ----------------

std::string maskApiKey(const std::string &k) {
  if (k.empty()) return "";
  const std::string tail = k.size() > 4 ? k.substr(k.size() - 4) : k;
  return "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2" + tail;
}

uint64_t driveCachedCount(const std::string &folderId) {
  uint64_t n = 0;
  if (driveCacheFileCount(driveCachePath(appDirectory + "/data", folderId), n))
    return n;
  return 0;
}

void finishDriveNet(const std::string &title, const std::string &msg) {
  std::lock_guard<std::mutex> lk(driveNetMutex);
  driveNetTitle = title;
  driveNetMsg = msg;
  driveNetDone = true;
  driveNetRunning = false;
}

void startDriveRescan() {
  if (driveNetRunning) {
    showDialog("Đang xử lý", "Vui lòng chờ tác vụ Drive hiện tại xong.");
    return;
  }
  const std::string key = resolveDriveApiKey();
  if (key.empty()) {
    showDialog("Thiếu API key",
               "Vào Cài đặt \u2192 Nhập API key Drive\n(hoặc đặt env "
               "GS_DRIVE_API_KEY).");
    return;
  }
  if (gDriveLinks.empty()) {
    showDialog("Chưa có nguồn", "Thêm link Drive trước đã.");
    return;
  }
  const std::vector<std::string> links = gDriveLinks;  // snapshot for thread
  driveNetRunning = true;
  driveNetProgress = 0;
  driveNetDone = false;
  std::thread([key, links] {
    DriveClient client(key, appDirectory);
    std::map<std::string, std::string> sysMap;
    loadSystemMap(appDirectory + "/config/system_map.json", sysMap);
    std::string summary;
    uint64_t totalFiles = 0;
    bool allOk = true;
    driveNetCancel = false;
    for (size_t i = 0; i < links.size(); ++i) {
      const std::string id = parseDriveFolderId(links[i]);
      if (id.empty()) {
        summary += "Nguồn " + std::to_string(i + 1) + ": link lỗi, bỏ qua\n";
        allOk = false;
        continue;
      }
      std::vector<DriveFile> files;
      DriveError err;
      if (!client.listRecursive(id, sysMap, files, driveNetProgress, driveNetCancel, err)) {
        summary +=
            "Nguồn " + std::to_string(i + 1) + ": LỖI " + err.message + "\n";
        allOk = false;
        continue;
      }
      saveDriveCache(driveCachePath(appDirectory + "/data", id), files);
      totalFiles += files.size();
      summary += "Nguồn " + std::to_string(i + 1) + ": " +
                 std::to_string(files.size()) + " file\n";
    }
    summary += "Tổng: " + std::to_string(totalFiles) + " file (đã lưu cache).";
    driveNetRescanFiles = totalFiles;
    driveNetWasRescan = true;
    finishDriveNet(allOk ? "Quét Drive xong" : "Quét Drive (có lỗi)", summary);
    Logger::info("Drive rescan: " + std::to_string(totalFiles) + " files");
  }).detach();
}

void startDriveCheck() {
  if (driveNetRunning) {
    showDialog("Đang xử lý", "Vui lòng chờ tác vụ Drive hiện tại xong.");
    return;
  }
  const std::string key = resolveDriveApiKey();
  if (key.empty()) {
    showDialog("Thiếu API key",
               "Vào Cài đặt \u2192 Nhập API key Drive\n(hoặc đặt env "
               "GS_DRIVE_API_KEY).");
    return;
  }
  if (gDriveLinks.empty()) {
    showDialog("Chưa có nguồn", "Thêm link Drive trước đã.");
    return;
  }
  const std::vector<std::string> links = gDriveLinks;
  driveNetRunning = true;
  driveNetProgress = 0;
  driveNetDone = false;
  std::thread([key, links] {
    DriveClient client(key, appDirectory);
    std::string summary;
    bool allOk = true;
    for (size_t i = 0; i < links.size(); ++i) {
      const std::string id = parseDriveFolderId(links[i]);
      if (id.empty()) {
        summary += "Nguồn " + std::to_string(i + 1) + ": link lỗi\n";
        allOk = false;
        continue;
      }
      std::vector<DriveItem> kids;
      DriveError err;
      if (!client.listChildren(id, kids, err)) {
        summary +=
            "Nguồn " + std::to_string(i + 1) + ": LỖI " + err.message + "\n";
        allOk = false;
        continue;
      }
      size_t nDirs = 0;
      for (const auto &k : kids)
        if (k.isFolder) ++nDirs;
      summary += "Nguồn " + std::to_string(i + 1) + ": OK (" +
                 std::to_string(nDirs) + " thư mục, " +
                 std::to_string(kids.size() - nDirs) + " file)\n";
    }
    driveNetWasRescan = false;
    finishDriveNet(allOk ? "Kết nối OK" : "Kiểm tra (có lỗi)", summary);
  }).detach();
}

// ---------------- per-screen input ----------------

void handleScreenInput(const InputState &in) {
  switch (screen) {
  case Screen::Home:
    if (in.up) moveSelection(-1, kHomeActionCount, homeSel);
    if (in.down) moveSelection(1, kHomeActionCount, homeSel);
    if (in.a) {
      Screen target = kHomeActions[homeSel].target;
      if (homeSel == 2) {  // nut "Dong bo": chua co cache Drive -> sang Drive
        uint64_t cached = 0;
        for (const auto &link : gDriveLinks) {
          const std::string fid = parseDriveFolderId(link);
          if (!fid.empty()) cached += driveCachedCount(fid);
        }
        target = cached > 0 ? Screen::Sync : Screen::Drive;
      }
      if (target == Screen::Sync) syncTab = 1;  // mặc định tab "Máy thiếu"
      screen = target;
    }
    if (in.y) startDriveRescan();
    if (in.b) running = false;
    break;
  case Screen::Library: {
    if (!libTabs.empty()) {
      const int n = static_cast<int>(libTabs.size());
      if (in.left) {
        libTab = (libTab + n - 1) % n;
        refreshLibView();
      }
      if (in.right) {
        libTab = (libTab + 1) % n;
        refreshLibView();
      }
    }
    if (!libView.empty()) {
      const int n = static_cast<int>(libView.size());
      if (in.up) libSel = (libSel + n - 1) % n;
      if (in.down) libSel = (libSel + 1) % n;
      const int rowH = Y(56), viewH = Y(430);
      const int sY = libSel * rowH;
      if (sY < libScroll) libScroll = sY;
      if (sY + rowH > libScroll + viewH) libScroll = sY + rowH - viewH;
      if (libScroll < 0) libScroll = 0;
    }
    if (in.x) openKeyboard("Tìm kiếm game", libQuery, KbMode::Search);
    if (in.y) startScan();
    if (in.b) {
      if (libSearching) {
        libSearching = false;
        libQuery.clear();
        refreshLibView();
      } else {
        screen = Screen::Home;
      }
    }
    break;
  }
  case Screen::Drive: {
    const int total = static_cast<int>(gDriveLinks.size()) + 3;
    if (in.up) moveSelection(-1, total, driveSel);
    if (in.down) moveSelection(1, total, driveSel);
    if (in.a) {
      const int nLinks = static_cast<int>(gDriveLinks.size());
      if (driveSel == nLinks) {
        openKeyboard("Dán link Drive", "", KbMode::AddLink);
      } else if (driveSel == nLinks + 1) {
        startDriveRescan();
      } else if (driveSel == nLinks + 2) {
        startDriveCheck();
      }
    }
    if (in.y) startDriveRescan();  // Y: quét Drive nhanh
    if (in.b) {
      if (driveNetRunning) {
        driveNetCancel = true;  // hủy quét Drive giữa chừng
      } else {
        screen = Screen::Home;
      }
    }
    break;
  }
  case Screen::Sync: {
    if (syncMode == SyncMode::Queue) {
      if (in.a && gQueue) {
        if (gQueue->paused())
          gQueue->resume();
        else
          gQueue->pause();
      }
      if (in.b) {
        if (gQueue) gQueue->cancel();
        gQueue.reset();
        syncMode = SyncMode::Compare;
        syncDirty = true;
      }
      break;
    }
    if (in.left) {
      syncTab = (syncTab + kSyncTabCount - 1) % kSyncTabCount;
      syncSel = 0;
      syncScroll = 0;
    }
    if (in.right) {
      syncTab = (syncTab + 1) % kSyncTabCount;
      syncSel = 0;
      syncScroll = 0;
    }
    {
      const std::vector<int> view = buildSyncView();
      const int n = static_cast<int>(view.size());
      if (n > 0) {
        if (in.up) syncSel = (syncSel + n - 1) % n;
        if (in.down) syncSel = (syncSel + 1) % n;
        if (in.x) {
          std::lock_guard<std::mutex> lk(gDiffMutex);
          const DiffEntry &e = gDiff[view[syncSel]];
          const std::string k = syncRowKey(e);
          if (syncMarked.count(k) > 0)
            syncMarked.erase(k);
          else
            syncMarked.insert(k);
        }
        if (in.a) {
          DiffEntry e;
          {
            std::lock_guard<std::mutex> lk(gDiffMutex);
            e = gDiff[view[syncSel]];
          }
          downloadSingleEntry(e);  // bấm A vào dòng để tải luôn
        }
      }
    }
    if (in.y) startSyncFlow();
    if (in.start) startFullSync();
    if (in.b) screen = Screen::Home;
    break;
  }
  case Screen::Fix: {
    size_t n = 0;
    {
      std::lock_guard<std::mutex> lk(gFixMutex);
      n = gFix.size();
    }
    if (in.up) moveSelection(-1, static_cast<int>(n), fixSel);
    if (in.down) moveSelection(1, static_cast<int>(n), fixSel);
    if (in.x) {
      std::lock_guard<std::mutex> lk(gFixMutex);
      if (fixSel >= 0 && fixSel < static_cast<int>(gFix.size())) {
        const FixEntry &e = gFix[fixSel];
        if (e.kind == FixKind::Move) {
          const std::string k = fixRowKey(e);
          if (fixMarked.count(k) > 0)
            fixMarked.erase(k);
          else
            fixMarked.insert(k);
        }
      }
    }
    if (in.a) {
      FixEntry e;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lk(gFixMutex);
        if (fixSel >= 0 && fixSel < static_cast<int>(gFix.size())) {
          e = gFix[fixSel];
          ok = true;
        }
      }
      if (ok) {
        if (e.kind != FixKind::Move) {
          showDialog("Không di chuyển", e.note + "\nNhấn A để đóng.");
        } else {
          char b[512];
          snprintf(b, sizeof b, "%s\n\n%s  →  %s\n\nNhấn A để đồng ý, B để hủy.",
                   e.name.c_str(),
                   e.system.empty() ? "(gốc Roms)" : e.system.c_str(),
                   e.expected.c_str());
          showConfirm("Di chuyển file?", b, [e] { doFixApply({e}); });
        }
      }
    }
    if (in.y) {
      std::vector<FixEntry> marked;
      {
        std::lock_guard<std::mutex> lk(gFixMutex);
        for (const auto &e : gFix)
          if (e.kind == FixKind::Move && fixMarked.count(fixRowKey(e)) > 0)
            marked.push_back(e);
      }
      if (marked.empty()) {
        showDialog("Di chuyển", "Chưa đánh dấu dòng nào.\nNhấn X để đánh dấu "
                                "các file cần di chuyển.\nNhấn A để đóng.");
      } else {
        char b[512];
        snprintf(b, sizeof b,
                 "Di chuyển %zu mục đã đánh dấu về đúng thư mục?\n\nNhấn A để "
                 "đồng ý, B để hủy.",
                 marked.size());
        showConfirm("Di chuyển hàng loạt?", b,
                    [marked] { doFixApply(marked); });
      }
    }
    if (in.start) startScan();
    if (in.b) screen = Screen::Home;
    break;
  }
  case Screen::Settings:
    if (in.up) moveSelection(-1, 5, settingsSel);
    if (in.down) moveSelection(1, 5, settingsSel);
    if (in.a) {
      if (settingsSel == 0) {
        const std::string p = appDirectory + "/data/.no-ota";
        if (fileExists(p))
          remove(p.c_str());
        else {
          std::ofstream f(p);
          f << "1\n";
        }
      } else if (settingsSel == 1) {
        const std::string p = appDirectory + "/data/.no-intro";
        if (fileExists(p))
          remove(p.c_str());
        else {
          std::ofstream f(p);
          f << "1\n";
        }
      } else if (settingsSel == 2) {
        showDialog("Kết nối WiFi",
                   "Hãy mở app WiFi của máy để kết\nnối mạng, rồi quay lại "
                   "Trimui-GameSync.\nNhấn A để đóng.");
      } else if (settingsSel == 3) {
        openKeyboard("Nhập API key Drive",
                     AppConfig::instance().getSetting("drive_api_key"),
                     KbMode::AddApiKey);
      } else if (settingsSel == 4) {
        int n = 0;
        DIR *d = opendir((appDirectory + "/data").c_str());
        if (d) {
          struct dirent *e;
          while ((e = readdir(d)) != nullptr) {
            const std::string nm = e->d_name;
            if (nm.rfind("drive_cache_", 0) == 0 && nm.size() > 4 &&
                nm.compare(nm.size() - 4, 4, ".bin") == 0) {
              if (remove((appDirectory + "/data/" + nm).c_str()) == 0) ++n;
            }
          }
          closedir(d);
        }
        char msg[128];
        snprintf(msg, sizeof msg, "Đã xóa %d file cache Drive.", n);
        showDialog("Xóa cache Drive", msg);
      }
    }
    if (in.b) screen = Screen::Home;
    break;
  case Screen::About:
    if (in.up || in.down) aboutSel = 0;
    if (in.a && aboutSel == 0) screen = Screen::Donate;
    if (in.b) screen = Screen::Home;
    break;
  case Screen::Donate:
    if (in.b) screen = Screen::About;
    break;
  }
}

void render() {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  fillRect(0, 0, scrW, scrH, color(15, 18, 24));
  drawHeader();
  switch (screen) {
  case Screen::Home: drawHome(); break;
  case Screen::Library: drawLibrary(); break;
  case Screen::Drive: drawDrive(); break;
  case Screen::Sync: drawSync(); break;
  case Screen::Fix: drawFix(); break;
  case Screen::Settings: drawSettings(); break;
  case Screen::About: drawAbout(); break;
  case Screen::Donate: drawDonate(); break;
  }
  if (dlg.open) drawDialog();
  if (cfm.open) drawConfirm();
  if (vkey.open) drawKeyboard();
  SDL_RenderPresent(renderer);
}

// ---------------- init / shutdown ----------------

bool init() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) !=
      0) {
    return false;
  }
  // Detect the real display size (chiaki-ng style). Brick Pro = 1024x768,
  // Smart Pro S = 1280x720. Fall back to 1024x768 when unreadable.
  scrW = kBaseW;
  scrH = kBaseH;
  SDL_DisplayMode dm;
  if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    scrW = dm.w;
    scrH = dm.h;
  }
  if (scrW < 800) scrW = kBaseW;
  if (scrH < 600) scrH = kBaseH;
  // Uniform scale: never stretch, center the 1024x768 logical UI.
  uiScale = std::min(scrW / static_cast<float>(kBaseW),
                     scrH / static_cast<float>(kBaseH));
  uiOX = static_cast<int>((scrW - kBaseW * uiScale) / 2.0f);
  uiOY = static_cast<int>((scrH - kBaseH * uiScale) / 2.0f);
  Logger::info("UI: screen " + std::to_string(scrW) + "x" +
               std::to_string(scrH) + " scale " + std::to_string(uiScale));
  if (TTF_Init() != 0) return false;
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  window = SDL_CreateWindow("Trimui-GameSync", SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED, scrW, scrH,
                            SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP);
  if (!window) return false;
  renderer = SDL_CreateRenderer(window, -1,
                                SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC);
  if (!renderer)
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!renderer) return false;
  SDL_RenderSetLogicalSize(renderer, scrW, scrH);
  const std::string fontPath = appDirectory + "/assets/NotoSans-Regular.ttf";
  const int fontSize = std::max(12, static_cast<int>(25 * uiScale));
  const int fontSmallSize = std::max(10, static_cast<int>(18 * uiScale));
  font = TTF_OpenFont(fontPath.c_str(), fontSize);
  fontSmall = TTF_OpenFont(fontPath.c_str(), fontSmallSize);
  if (!font || !fontSmall) return false;
  for (int index = 0; index < SDL_NumJoysticks(); ++index) {
    if (SDL_IsGameController(index)) {
      controller = SDL_GameControllerOpen(index);
      if (controller) break;
    }
  }
  if (!controller && SDL_NumJoysticks() > 0) joystick = SDL_JoystickOpen(0);
  // P1 data: systems + drive sources (fast, local files only).
  gSystems = loadSystems(appDirectory);
  gDriveLinks = loadDriveLinks(appDirectory);
  Logger::info("Loaded " + std::to_string(gSystems.size()) + " systems, " +
               std::to_string(gDriveLinks.size()) + " drive sources");
  // P2: cached library scan loads instantly; user rescans with Y.
  {
    ScanResult cached;
    if (loadScanCache(appDirectory + "/data/scan_cache.bin", cached)) {
      std::lock_guard<std::mutex> lk(gScanMutex);
      gScan = std::move(cached);
      scanDone = true;
      scanFileCount = gScan.files.size();
      uint64_t tb = 0;
      for (const auto &f : gScan.files) tb += f.size;
      scanTotalBytes = tb;
      Logger::info("Loaded scan cache: " +
                   std::to_string(gScan.files.size()) + " files");
    }
  }
  rebuildLibTabs();
  refreshLibView();
  return true;
}

void shutdownApp() {
  if (qrTex) SDL_DestroyTexture(qrTex);
  if (controller) SDL_GameControllerClose(controller);
  if (joystick) SDL_JoystickClose(joystick);
  if (font) TTF_CloseFont(font);
  if (fontSmall) TTF_CloseFont(fontSmall);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (window) SDL_DestroyWindow(window);
  TTF_Quit();
  SDL_Quit();
}

// Copy the shipped DriveLinks.txt seed into data/ on first launch so the
// user can edit it freely (OTA never touches data/).
void seedDriveLinks() {
  const std::string dataLinks = appDirectory + "/data/DriveLinks.txt";
  std::ifstream probe(dataLinks);
  if (probe.good()) return;
  std::ifstream src(appDirectory + "/DriveLinks.txt", std::ios::binary);
  if (!src.good()) return;
  std::ofstream out(dataLinks, std::ios::binary | std::ios::trunc);
  out << src.rdbuf();
  Logger::info("Seeded data/DriveLinks.txt from shipped default");
}

} // namespace GameSync

int main(int argc, char **argv) {
  using namespace GameSync;
  // CLI modes for OTA (no SDL needed): verify an Ed25519-signed manifest.
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--verify-manifest" && i + 1 < argc) {
      return OtaVerifyManifest(argv[i + 1]) ? 0 : 1;
    }
    if (arg == "--version") {
      printf("%s\n", APP_VERSION);
      return 0;
    }
  }
  if (argc > 0) {
    std::string executable = argv[0];
    const size_t slash = executable.find_last_of('/');
    if (slash != std::string::npos) appDirectory = executable.substr(0, slash);
    if (appDirectory.size() >= 4 &&
        appDirectory.substr(appDirectory.size() - 4) == "/bin") {
      appDirectory.resize(appDirectory.size() - 4);
    }
  }
  appVersion = readRuntimeVersion(appDirectory);
  AppConfig::instance().setDataDirectory(appDirectory + "/data");
  Logger::setLogPath(appDirectory + "/gamesync.log");
  seedDriveLinks();
  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    Logger::error("Unable to initialize libcurl");
    return 1;
  }
  if (!init()) {
    Logger::error(std::string("SDL init failed: ") + SDL_GetError());
    shutdownApp();
    curl_global_cleanup();
    return 1;
  }

  // NLK intro right after video init, before the main UI.
  playIntro(renderer, scrW, scrH, appDirectory + "/assets/NotoSans-Regular.ttf",
            appDirectory);

  while (running) {
    // OTA tự động (học từ LottoForecast-Pro): ota-update.sh ghi "done <ver>"
    // vào .ota-status khi cài xong -> thoát 42 để launch.sh chạy lại bản mới.
    {
      static Uint32 lastOtaCheck = 0;
      Uint32 nowTick = SDL_GetTicks();
      if (nowTick - lastOtaCheck > 5000) {
        lastOtaCheck = nowTick;
        std::ifstream ost(appDirectory + "/.ota-status");
        if (ost) {
          std::string ostLine;
          std::getline(ost, ostLine);
          if (ostLine.rfind("done ", 0) == 0) {
            Logger::info("OTA xong (" + ostLine + ") -> restart app (exit 42)");
            if (std::getenv("TRIMUI_GS_LAUNCH_LOOP")) {
              std::exit(42);
            }
            std::remove((appDirectory + "/.ota-status").c_str());
          }
        }
      }
    }
    if (scanFinishedFlag.exchange(false)) {
      rebuildLibTabs();
      refreshLibView();
      syncDirty = true;
      fixDirty = true;
      if (pendingFullSync) {
        // Chuỗi "Đồng bộ tất cả": quét máy xong → quét Drive.
        startDriveRescan();
        if (!driveNetRunning) pendingFullSync = false;  // không start được
      }
    }
    if (driveNetDone.exchange(false)) {
      std::string t, m;
      {
        std::lock_guard<std::mutex> lk(driveNetMutex);
        t = driveNetTitle;
        m = driveNetMsg;
      }
      const bool wasRescan = driveNetWasRescan.exchange(false);
      if (pendingFullSync.exchange(false)) {
        // Chuỗi "Đồng bộ tất cả": Drive xong → so sánh → báo máy thiếu/drive thiếu.
        recomputeDiff();
        syncTab = 1;  // về tab "Máy thiếu"
        screen = Screen::Sync;
        uint64_t nRemote = 0, nLocal = 0, nSynced = 0;
        {
          std::lock_guard<std::mutex> lk(gDiffMutex);
          for (const auto &e : gDiff) {
            if (e.status == DiffStatus::REMOTE_ONLY)
              ++nRemote;
            else if (e.status == DiffStatus::LOCAL_ONLY)
              ++nLocal;
            else if (e.status == DiffStatus::SYNCED)
              ++nSynced;
          }
        }
        char b[320];
        snprintf(b, sizeof b,
                 "↓ Máy thiếu: %llu game\n↑ Drive thiếu: %llu game\nĐã đồng bộ: "
                 "%llu game\n\nBấm A vào từng dòng để tải ngay.",
                 (unsigned long long)nRemote, (unsigned long long)nLocal,
                 (unsigned long long)nSynced);
        showDialog("Đồng bộ xong", b);
      } else if (wasRescan) {
        // Vua dong bo Drive xong: hoi co sang man hinh So sanh khong.
        const uint64_t n = driveNetRescanFiles.load();
        std::string b = "Đã lấy " + std::to_string(n) + " file từ Drive.\n\n" +
                        m + "\n\nSang màn hình So sánh để tải game về?";
        showConfirm(t, b, [] {
          screen = Screen::Sync;
          syncTab = 1;
          syncDirty = true;
        });
      } else {
        showDialog(t, m);
      }
      syncDirty = true;
    }
    const InputState input = pollInput();
    if (vkey.open) {
      handleKeyboardInput(input);
    } else if (cfm.open) {
      if (input.a) {
        cfm.open = false;
        auto cb = std::move(cfm.onYes);
        if (cb) cb();
      } else if (input.b) {
        cfm.open = false;
        cfm.onYes = nullptr;
      }
    } else if (dlg.open) {
      if (input.a || input.b) dlg.open = false;
    } else {
      handleScreenInput(input);
    }
    if (vkey.done && !vkey.open) {
      onKeyboardDone();
      vkey.done = false;
    }
    if (syncMode == SyncMode::Queue && gQueue && gQueue->finished() &&
        !queueDoneHandled) {
      queueDoneHandled = true;
      const int code = gQueue->lastHttpCode();
      const std::vector<TaskView> views = gQueue->snapshot();
      int doneN = 0, errN = 0;
      for (const auto &v : views) {
        if (v.state == TaskState::Done) ++doneN;
        if (v.state == TaskState::Error) ++errN;
      }
      if ((code == 401 || code == 403) && !queueAuthShown) {
        queueAuthShown = true;
        char b[192];
        snprintf(b, sizeof b,
                 "Drive từ chối truy cập (HTTP %d).\nVao Cai dat -> Nhap API "
                 "key Drive.",
                 code);
        showDialog("Lỗi API key", b);
      } else {
        char b[256];
        snprintf(b, sizeof b, "Xong %d/%zu file%s.\nNhấn A để đóng.", doneN,
                 views.size(),
                 errN ? (" (lỗi " + std::to_string(errN) + " file)").c_str()
                      : "");
        showDialog("Đồng bộ xong", b);
      }
    }
    render();
    SDL_Delay(16);
  }

  shutdownApp();
  curl_global_cleanup();
  return 0;
}
