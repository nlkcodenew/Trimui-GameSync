// Fixer.cpp — P5: misplaced-file detection + confirmed moves.
// Only dirent/stat/fstream: builds natively for the WSL logic test.
#include "Fixer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace GameSync {
namespace {

// Canonical extension -> system table. ORDER MATTERS: it drives the
// display table grouping in extMapDisplayRows().
const std::pair<const char *, const char *> kExtSystem[] = {
    {".nes", "FC"}, {".fds", "FC"},                       //
    {".sfc", "SFC"}, {".smc", "SFC"},                     //
    {".gb", "GB"}, {".gbc", "GBC"}, {".gba", "GBA"},      //
    {".md", "MD"}, {".gen", "MD"}, {".smd", "MD"},         //
    {".sms", "MS"}, {".gg", "GG"},                        //
    {".pce", "PCE"},                                      //
    {".a26", "ATARI2600"},                                //
    {".nds", "NDS"},                                      //
    {".cue", "PS"}, {".bin", "PS"}, {".m3u", "PS"}, {".pbp", "PS"},
};

const char *kAmbiguousExts[] = {".iso", ".cso", ".chd",
                                ".zip", ".7z",  ".rar"};

std::string toLowerStr(std::string s) {
  for (char &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
  return s;
}

// Extensions that take part in multi-disc grouping.
bool isDiscExt(const std::string &ext) {
  return ext == ".cue" || ext == ".bin" || ext == ".m3u" || ext == ".pbp";
}

// PortMaster port data folders (ports/, A30PORTS/): not ROM library files —
// their data files (.md metadata, .bin assets, ...) must never be "fixed".
bool isPortDataFolder(const std::string &system) {
  const std::string l = toLowerStr(system);
  return l == "ports" || l == "a30ports";
}

// "SFC/sub/game.nes" -> "SFC/sub" ; "game.nes" -> "".
std::string parentDir(const std::string &relPath) {
  const size_t p = relPath.rfind('/');
  return (p == std::string::npos) ? std::string() : relPath.substr(0, p);
}

// "game.nes" -> "game".
std::string stemOf(const std::string &name, const std::string &ext) {
  if (!ext.empty() && name.size() > ext.size() &&
      toLowerStr(name.substr(name.size() - ext.size())) == ext)
    return name.substr(0, name.size() - ext.size());
  return name;
}

bool pathExists(const std::string &p) {
  struct stat st{};
  return stat(p.c_str(), &st) == 0;
}

// mkdir -p. Returns false on hard error (not on EEXIST).
bool mkdirP(const std::string &path) {
  if (path.empty() || pathExists(path)) return true;
  std::string cur;
  size_t i = 0;
  if (path[0] == '/') {
    cur = "/";
    i = 1;
  }
  while (i <= path.size()) {
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) {
      if (!cur.empty() && cur.back() != '/') cur += '/';
      cur += path.substr(i, j - i);
      if (!pathExists(cur) && mkdir(cur.c_str(), 0755) != 0 &&
          !pathExists(cur))
        return false;
    }
    i = j + 1;
  }
  return true;
}

// Strip the first path component ("SFC/sub/game.nes" -> "sub/game.nes").
// Returns "" when unsafe (no '/', ".." anywhere).
std::string innerRel(const std::string &relPath) {
  const size_t p = relPath.find('/');
  if (p == std::string::npos) return "";
  const std::string inner = relPath.substr(p + 1);
  if (inner.empty() || inner.find("..") != std::string::npos) return "";
  return inner;
}

}  // namespace

std::string fileExtLower(const std::string &name) {
  const size_t p = name.rfind('.');
  if (p == std::string::npos || p + 1 >= name.size()) return "";
  return toLowerStr(name.substr(p));
}

const char *expectedSystemForExt(const std::string &extLower) {
  for (const auto &kv : kExtSystem)
    if (extLower == kv.first) return kv.second;
  return "";
}

bool isAmbiguousExt(const std::string &extLower) {
  for (const char *e : kAmbiguousExts)
    if (extLower == e) return true;
  return false;
}

bool isCompatiblePlacement(const std::string &fileSystem,
                           const std::string &folderSystem) {
  // A Game Boy Color runs Game Boy games: .gb inside GBC/ is fine.
  if (fileSystem == "GB" && folderSystem == "GBC") return true;
  return false;
}

std::vector<std::pair<std::string, std::string>> extMapDisplayRows() {
  std::vector<std::pair<std::string, std::string>> rows;
  for (const auto &kv : kExtSystem) {
    if (!rows.empty() && rows.back().second == kv.second) {
      rows.back().first += " / ";
      rows.back().first += kv.first;
    } else {
      rows.emplace_back(kv.first, kv.second);
    }
  }
  rows.emplace_back(".iso / .cso / .chd", "không rõ — xem tay");
  rows.emplace_back(".zip / .7z / .rar", "giữ nguyên — xem tay");
  return rows;
}

std::vector<FixEntry> findMisplaced(const ScanResult &scan) {
  std::vector<FixEntry> out;
  struct Candidate {
    FixEntry e;
    std::string dirKey;   // parent dir (with system prefix)
    std::string stemKey;  // lowercased stem, for disc grouping
  };
  std::vector<Candidate> cands;

  for (const GameFile &f : scan.files) {
    if (isPortDataFolder(f.system)) continue;
    const std::string ext = fileExtLower(f.name);
    const char *expected = expectedSystemForExt(ext);
    if (expected[0] == '\0') {
      // Unknown or ambiguous extension.
      if (ext.empty() || isAmbiguousExt(ext)) {
        FixEntry e;
        e.system = f.system;
        e.relPath = f.relPath;
        e.name = f.name;
        e.ext = ext;
        e.kind = FixKind::WarnOnly;
        e.note = ext.empty() ? "Không có đuôi file — xem tay"
                             : "Không xác định được hệ máy — xem tay";
        e.size = f.size;
        out.push_back(std::move(e));
      }
      continue;
    }
    const std::string expStr = expected;
    if (f.system == expStr) continue;  // already in the right folder
    if (isCompatiblePlacement(expStr, f.system)) {
      FixEntry e;
      e.system = f.system;
      e.relPath = f.relPath;
      e.name = f.name;
      e.ext = ext;
      e.kind = FixKind::WarnOnly;
      e.note = "Chạy được ở thư mục này (" + expStr + "→" + f.system +
               ") — không cần move";
      e.size = f.size;
      out.push_back(std::move(e));
      continue;
    }
    FixEntry e;
    e.system = f.system;
    e.relPath = f.relPath;
    e.name = f.name;
    e.ext = ext;
    e.expected = expStr;
    e.kind = FixKind::Move;
    e.size = f.size;
    cands.push_back({std::move(e), parentDir(f.relPath),
                     toLowerStr(stemOf(f.name, ext))});
  }

  // Multi-disc grouping: .cue/.bin/.m3u/.pbp sharing one stem in one dir
  // always move together, never split.
  std::sort(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) {
    if (a.e.system != b.e.system) return a.e.system < b.e.system;
    if (a.dirKey != b.dirKey) return a.dirKey < b.dirKey;
    if (a.stemKey != b.stemKey) return a.stemKey < b.stemKey;
    return a.e.relPath < b.e.relPath;
  });
  for (size_t i = 0; i < cands.size();) {
    size_t j = i + 1;
    const bool groupable = isDiscExt(cands[i].e.ext);
    while (j < cands.size() && groupable && isDiscExt(cands[j].e.ext) &&
           cands[j].e.system == cands[i].e.system &&
           cands[j].dirKey == cands[i].dirKey &&
           cands[j].stemKey == cands[i].stemKey &&
           cands[j].e.expected == cands[i].e.expected)
      ++j;
    if (j - i >= 2) {
      FixEntry g;
      g.system = cands[i].e.system;
      g.relPath = cands[i].dirKey.empty()
                      ? cands[i].stemKey
                      : cands[i].dirKey + "/" + cands[i].stemKey;
      char nm[256];
      snprintf(nm, sizeof nm, "%s.* (%zu files)",
               stemOf(cands[i].e.name, cands[i].e.ext).c_str(), j - i);
      g.name = nm;
      g.expected = cands[i].e.expected;
      g.kind = FixKind::Move;
      g.isGroup = true;
      for (size_t k = i; k < j; ++k) {
        g.size += cands[k].e.size;
        g.members.push_back(std::move(cands[k].e));
      }
      out.push_back(std::move(g));
    } else {
      out.push_back(std::move(cands[i].e));
    }
    i = j;
  }

  std::sort(out.begin(), out.end(), [](const FixEntry &a, const FixEntry &b) {
    if (a.system != b.system) return a.system < b.system;
    return a.relPath < b.relPath;
  });
  return out;
}

FixResult applyFixes(const std::vector<FixEntry> &entries,
                     const std::string &romsDir, const std::string &appDir) {
  FixResult r;
  std::string logPath = appDir + "/data/fix.log";
  std::ofstream log;
  bool logOpen = false;

  auto ensureLog = [&]() {
    if (!logOpen) {
      log.open(logPath, std::ios::app);
      logOpen = log.is_open();
    }
  };

  std::vector<const FixEntry *> jobs;
  for (const FixEntry &e : entries) {
    if (e.kind != FixKind::Move) continue;
    if (e.isGroup) {
      for (const FixEntry &m : e.members) jobs.push_back(&m);
    } else {
      jobs.push_back(&e);
    }
  }

  for (const FixEntry *m : jobs) {
    const std::string inner = innerRel(m->relPath);
    if (inner.empty() || m->expected.empty()) {
      ++r.skipped;
      r.errors.push_back("Đường dẫn không an toàn: " + m->relPath);
      continue;
    }
    const std::string src = romsDir + "/" + m->relPath;
    const std::string dstDir = romsDir + "/" + m->expected + "/" +
                               parentDir(inner);
    const std::string dst = romsDir + "/" + m->expected + "/" + inner;
    if (!mkdirP(dstDir)) {
      ++r.skipped;
      r.errors.push_back("Không tạo được thư mục: " + dstDir);
      continue;
    }
    if (pathExists(dst)) {
      ++r.skipped;
      r.errors.push_back("Đích đã tồn tại (không ghi đè): " + dst);
      continue;
    }
    if (rename(src.c_str(), dst.c_str()) != 0) {
      ++r.skipped;
      r.errors.push_back(std::string("Move lỗi: ") + m->relPath + " (" +
                         strerror(errno) + ")");
      continue;
    }
    ++r.moved;
    ensureLog();
    if (logOpen) {
      std::time_t t = std::time(nullptr);
      char ts[20];
      std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
      log << ts << " | " << m->relPath << " -> " << m->expected << "/"
          << inner << "\n";
    }
  }
  return r;
}

std::string fixSummary(const std::vector<FixEntry> &entries) {
  int nMove = 0, nWarn = 0, nGroups = 0;
  for (const auto &e : entries) {
    if (e.kind == FixKind::Move) {
      ++nMove;
      if (e.isGroup) ++nGroups;
    } else {
      ++nWarn;
    }
  }
  std::string s = "COUNTS MOVE=" + std::to_string(nMove) + " WARN=" +
                  std::to_string(nWarn) + " GROUPS=" + std::to_string(nGroups) +
                  " FILES=" + std::to_string(entries.size()) + "\n";
  char line[1024];
  for (const auto &e : entries) {
    if (e.kind == FixKind::Move && e.isGroup) {
      const std::string dir = parentDir(e.relPath);
      const std::string stem = e.relPath.substr(dir.empty() ? 0 : dir.size() + 1);
      snprintf(line, sizeof line, "G|%s|%s|%s|%s|%zu|%llu\n", e.system.c_str(),
               dir.c_str(), stem.c_str(), e.expected.c_str(), e.members.size(),
               (unsigned long long)e.size);
    } else if (e.kind == FixKind::Move) {
      snprintf(line, sizeof line, "M|%s|%s|%s|%llu\n", e.system.c_str(),
               e.relPath.c_str(), e.expected.c_str(),
               (unsigned long long)e.size);
    } else {
      snprintf(line, sizeof line, "W|%s|%s|%s|%s\n", e.system.c_str(),
               e.relPath.c_str(), e.ext.c_str(), e.note.c_str());
    }
    s += line;
  }
  return s;
}

}  // namespace GameSync
