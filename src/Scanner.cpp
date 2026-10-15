// Scanner.cpp — P2: local game library scan (platform-independent logic;
// only dirent/stat, so it also builds natively for the WSL logic test).
#include "Scanner.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace GameSync {

std::string defaultRomsDir() {
  const char *env = getenv("GS_ROMS_DIR");
  if (env && env[0] != '\0') return std::string(env);
  return "/mnt/SDCARD/Roms";
}

namespace {

bool isHidden(const char *name) { return name[0] == '.'; }

void scanDir(const std::string &dir, const std::string &relBase,
             const std::string &system, ScanResult &out,
             std::atomic<uint64_t> &progress, const std::atomic<bool> &cancel,
             int depth) {
  if (cancel || depth > 16) return;
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (cancel) break;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (isHidden(e->d_name)) continue;
    const std::string full = dir + "/" + e->d_name;
    const std::string rel = relBase.empty() ? e->d_name : relBase + "/" + e->d_name;
    struct stat st{};
    // lstat: never follow symlinks (avoid loops / escaping Roms/).
    if (lstat(full.c_str(), &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue;
    if (S_ISDIR(st.st_mode)) {
      scanDir(full, rel, system, out, progress, cancel, depth + 1);
    } else if (S_ISREG(st.st_mode)) {
      GameFile f;
      f.system = system;
      f.relPath = rel;
      f.name = e->d_name;
      f.size = static_cast<uint64_t>(st.st_size);
      f.mtime = static_cast<uint64_t>(st.st_mtime);
      out.totalBytes += f.size;
      out.files.push_back(std::move(f));
      ++progress;
    }
  }
  closedir(d);
}

// ---- cache format ----
// magic "GSSCAN01" (8 bytes), u64 file count, then per file:
//   u32 len + bytes (system), u32 len + bytes (relPath), u32 len + bytes (name),
//   u64 size, u64 mtime. All integers little-endian.

const char kMagic[8] = {'G', 'S', 'S', 'C', 'A', 'N', '0', '1'};

void putU32(std::string &o, uint32_t v) {
  o.push_back(static_cast<char>(v & 0xFF));
  o.push_back(static_cast<char>((v >> 8) & 0xFF));
  o.push_back(static_cast<char>((v >> 16) & 0xFF));
  o.push_back(static_cast<char>((v >> 24) & 0xFF));
}

void putU64(std::string &o, uint64_t v) {
  for (int i = 0; i < 8; ++i)
    o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void putStr(std::string &o, const std::string &s) {
  putU32(o, static_cast<uint32_t>(s.size()));
  o.append(s);
}

struct Reader {
  const char *d;
  size_t n;
  size_t pos = 0;
  bool bytes(void *o, size_t k) {
    if (pos + k > n) return false;
    memcpy(o, d + pos, k);
    pos += k;
    return true;
  }
  bool u32(uint32_t &v) {
    unsigned char b[4];
    if (!bytes(b, 4)) return false;
    v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
        (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
    return true;
  }
  bool u64(uint64_t &v) {
    unsigned char b[8];
    if (!bytes(b, 8)) return false;
    v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[i]) << (8 * i);
    return true;
  }
  bool str(std::string &s) {
    uint32_t len = 0;
    if (!u32(len)) return false;
    if (pos + len > n) return false;
    s.assign(d + pos, len);
    pos += len;
    return true;
  }
};

}  // namespace

void scanRoms(const std::string &root, ScanResult &out,
              std::atomic<uint64_t> &progress, const std::atomic<bool> &cancel) {
  out.files.clear();
  out.totalBytes = 0;
  DIR *d = opendir(root.c_str());
  if (!d) return;
  // Top-level entries decide the system code; files directly under root
  // get system "" (shown under "Khác" in the UI).
  std::vector<std::string> topDirs;
  struct dirent *e;
  while ((e = readdir(d)) != nullptr) {
    if (cancel) break;
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (isHidden(e->d_name)) continue;
    const std::string full = root + "/" + e->d_name;
    struct stat st{};
    if (lstat(full.c_str(), &st) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue;
    if (S_ISDIR(st.st_mode)) {
      topDirs.push_back(e->d_name);
    } else if (S_ISREG(st.st_mode)) {
      GameFile f;
      f.system = "";
      f.relPath = e->d_name;
      f.name = e->d_name;
      f.size = static_cast<uint64_t>(st.st_size);
      f.mtime = static_cast<uint64_t>(st.st_mtime);
      out.totalBytes += f.size;
      out.files.push_back(std::move(f));
      ++progress;
    }
  }
  closedir(d);
  for (const auto &t : topDirs) {
    if (cancel) break;
    scanDir(root + "/" + t, t, t, out, progress, cancel, 1);
  }
  std::sort(out.files.begin(), out.files.end(),
            [](const GameFile &a, const GameFile &b) {
              if (a.system != b.system) return a.system < b.system;
              return a.relPath < b.relPath;
            });
}

bool saveScanCache(const std::string &path, const ScanResult &res) {
  std::string o;
  o.append(kMagic, 8);
  putU64(o, static_cast<uint64_t>(res.files.size()));
  for (const auto &f : res.files) {
    putStr(o, f.system);
    putStr(o, f.relPath);
    putStr(o, f.name);
    putU64(o, f.size);
    putU64(o, f.mtime);
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(o.data(), static_cast<std::streamsize>(o.size()));
  return static_cast<bool>(out);
}

bool loadScanCache(const std::string &path, ScanResult &res) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  const std::string buf((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
  if (buf.size() < 16) return false;
  if (memcmp(buf.data(), kMagic, 8) != 0) return false;
  Reader r{buf.data(), buf.size()};
  r.pos = 8;
  uint64_t count = 0;
  if (!r.u64(count)) return false;
  if (count > 1000000) return false;  // sanity cap
  ScanResult tmp;
  tmp.files.reserve(static_cast<size_t>(count));
  for (uint64_t i = 0; i < count; ++i) {
    GameFile f;
    if (!r.str(f.system) || !r.str(f.relPath) || !r.str(f.name)) return false;
    if (!r.u64(f.size) || !r.u64(f.mtime)) return false;
    if (f.system.size() > 256 || f.relPath.size() > 4096 || f.name.size() > 1024)
      return false;
    tmp.totalBytes += f.size;
    tmp.files.push_back(std::move(f));
  }
  res = std::move(tmp);
  return true;
}

}  // namespace GameSync
