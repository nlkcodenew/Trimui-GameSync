// Sync — P4: compare local library vs Drive + background download queue.

#include "Sync.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>

namespace GameSync {
namespace {

// ---- small helpers ----

std::string toLower(std::string s) {
  for (auto &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
  return s;
}

// Path of the file inside its system folder.
// Local: relPath is "GBA/game.zip" (system prefix) or "game.zip" (root, "").
// Remote: relPath is "<Drive folder>/game.zip" or "game.zip" (root).
std::string pathInSystemLocal(const GameFile &f) {
  if (!f.system.empty()) {
    const size_t p = f.relPath.find('/');
    if (p != std::string::npos) return f.relPath.substr(p + 1);
  }
  return f.relPath;
}

std::string pathInSystemRemote(const DriveFile &f) {
  const size_t p = f.relPath.find('/');
  if (p != std::string::npos) return f.relPath.substr(p + 1);
  return f.relPath;
}

std::string dirName(const std::string &p) {
  const size_t s = p.rfind('/');
  return (s == std::string::npos) ? "" : p.substr(0, s);
}

std::string baseName(const std::string &p) {
  const size_t s = p.rfind('/');
  return (s == std::string::npos) ? p : p.substr(s + 1);
}

std::string stemOf(const std::string &name) {
  const size_t d = name.rfind('.');
  return (d == std::string::npos || d == 0) ? name : name.substr(0, d);
}

std::string extOf(const std::string &name) {
  const size_t d = name.rfind('.');
  if (d == std::string::npos || d + 1 >= name.size()) return "";
  return toLower(name.substr(d + 1));
}

// Multi-disc extensions that belong to one game (PS1 etc.).
bool isGroupExt(const std::string &ext) {
  return ext == "cue" || ext == "bin" || ext == "m3u" || ext == "pbp";
}

}  // namespace

const char *diffStatusLabel(DiffStatus s) {
  switch (s) {
    case DiffStatus::SYNCED: return "Đã đồng bộ";
    case DiffStatus::REMOTE_ONLY: return "↓ Máy thiếu";
    case DiffStatus::LOCAL_ONLY: return "↑ Drive thiếu";
    case DiffStatus::DIFFER: return "≠ Khác bản";
  }
  return "?";
}

std::vector<DiffEntry> compareLibrary(const ScanResult &local,
                                     const std::vector<DriveFile> &remote) {
  struct Side {
    bool hasLocal = false, hasRemote = false;
    uint64_t localSize = 0, remoteSize = 0;
    std::string remoteId, remoteMd5;
  };
  // key: system + '\x1f' + path-in-system
  std::map<std::string, Side> m;
  std::map<std::string, std::pair<std::string, std::string>> keyParts;
  for (const auto &f : local.files) {
    const std::string pis = pathInSystemLocal(f);
    const std::string k = f.system + '\x1f' + pis;
    keyParts[k] = std::make_pair(f.system, pis);
    Side &s = m[k];
    s.hasLocal = true;
    s.localSize = f.size;
  }
  for (const auto &f : remote) {
    const std::string pis = pathInSystemRemote(f);
    const std::string k = f.system + '\x1f' + pis;
    keyParts[k] = std::make_pair(f.system, pis);
    Side &s = m[k];
    s.hasRemote = true;
    s.remoteSize = f.size;
    s.remoteId = f.id;
    s.remoteMd5 = f.md5;
  }

  std::vector<DiffEntry> singles;
  singles.reserve(m.size());
  for (const auto &kv : m) {
    const Side &s = kv.second;
    DiffEntry e;
    e.system = keyParts[kv.first].first;
    e.relPath = keyParts[kv.first].second;
    e.name = baseName(e.relPath);
    e.localSize = s.localSize;
    e.remoteId = s.remoteId;
    e.remoteMd5 = s.remoteMd5;
    if (s.hasLocal && s.hasRemote) {
      e.status = (s.localSize == s.remoteSize) ? DiffStatus::SYNCED
                                              : DiffStatus::DIFFER;
      e.remoteSize = (e.status == DiffStatus::DIFFER) ? s.remoteSize : 0;
    } else if (s.hasRemote) {
      e.status = DiffStatus::REMOTE_ONLY;
      e.remoteSize = s.remoteSize;
    } else {
      e.status = DiffStatus::LOCAL_ONLY;
      e.remoteSize = 0;
    }
    singles.push_back(std::move(e));
  }

  // Multi-disc grouping: same (system, dir, stem), groupable ext, >= 2 files.
  std::map<std::string, std::vector<size_t>> groups;
  std::vector<char> grouped(singles.size(), 0);
  for (size_t i = 0; i < singles.size(); ++i) {
    if (!isGroupExt(extOf(singles[i].name))) continue;
    const std::string gk = singles[i].system + '\x1f' +
                           dirName(singles[i].relPath) + '\x1f' +
                           stemOf(singles[i].name);
    groups[gk].push_back(i);
  }
  std::vector<DiffEntry> rows;
  rows.reserve(singles.size());
  for (const auto &g : groups) {
    if (g.second.size() < 2) continue;
    DiffEntry grp;
    bool first = true;
    bool allSynced = true, allRemote = true, allLocal = true;
    for (size_t i : g.second) {
      grouped[i] = 1;
      const DiffEntry &e = singles[i];
      if (first) {
        grp.system = e.system;
        const std::string d = dirName(e.relPath);
        grp.relPath = d.empty() ? stemOf(e.name) : d + "/" + stemOf(e.name);
        grp.name = stemOf(e.name);
        grp.isGroup = true;
        first = false;
      }
      grp.members.push_back(e);
      grp.localSize += e.localSize;
      if (e.status == DiffStatus::REMOTE_ONLY ||
          e.status == DiffStatus::DIFFER)
        grp.remoteSize += e.remoteSize;
      if (e.status != DiffStatus::SYNCED) allSynced = false;
      if (e.status != DiffStatus::REMOTE_ONLY) allRemote = false;
      if (e.status != DiffStatus::LOCAL_ONLY) allLocal = false;
    }
    std::sort(grp.members.begin(), grp.members.end(),
              [](const DiffEntry &a, const DiffEntry &b) {
                return a.name < b.name;
              });
    grp.status = allSynced ? DiffStatus::SYNCED
               : allRemote ? DiffStatus::REMOTE_ONLY
               : allLocal  ? DiffStatus::LOCAL_ONLY
                           : DiffStatus::DIFFER;
    rows.push_back(std::move(grp));
  }
  for (size_t i = 0; i < singles.size(); ++i)
    if (!grouped[i]) rows.push_back(std::move(singles[i]));

  std::sort(rows.begin(), rows.end(), [](const DiffEntry &a, const DiffEntry &b) {
    if (a.system != b.system) return a.system < b.system;
    return a.relPath < b.relPath;
  });
  return rows;
}

std::string safeRomsPath(const std::string &romsDir, const std::string &system,
                         const std::string &relInSys) {
  if (system.empty() || system.size() > 64) return "";
  for (char c : system) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (!std::isalnum(u) && c != '_' && c != '-' && c != '+') return "";
  }
  if (relInSys.empty() || relInSys[0] == '/') return "";
  std::string joined;
  size_t i = 0;
  while (i < relInSys.size()) {
    size_t j = relInSys.find('/', i);
    if (j == std::string::npos) j = relInSys.size();
    std::string part = relInSys.substr(i, j - i);
    i = j + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") return "";  // fail closed: no traversal
    if (part.find('\\') != std::string::npos) return "";
    if (part.size() > 255) return "";
    if (!joined.empty()) joined.push_back('/');
    joined += part;
  }
  if (joined.empty()) return "";
  std::string base = romsDir;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + "/" + system + "/" + joined;
}

std::vector<DownloadTask> buildDownloadTasks(const std::vector<DiffEntry> &rows,
                                            const std::string &romsDir,
                                            int &skippedUnsafe) {
  skippedUnsafe = 0;
  std::vector<DownloadTask> out;
  auto pushMember = [&](const DiffEntry &e) {
    if (e.status != DiffStatus::REMOTE_ONLY &&
        e.status != DiffStatus::DIFFER)
      return;
    if (e.remoteId.empty()) {
      ++skippedUnsafe;
      return;
    }
    const std::string dest = safeRomsPath(romsDir, e.system, e.relPath);
    if (dest.empty()) {
      ++skippedUnsafe;
      return;
    }
    DownloadTask t;
    t.label = e.system.empty() ? e.name : e.system + "/" + e.name;
    t.fileId = e.remoteId;
    t.destPath = dest;
    t.size = e.remoteSize;
    t.md5 = e.remoteMd5;
    out.push_back(std::move(t));
  };
  for (const auto &r : rows) {
    if (r.isGroup) {
      for (const auto &mem : r.members) pushMember(mem);
    } else {
      pushMember(r);
    }
  }
  return out;
}

// ---------------- DownloadQueue ----------------

DownloadQueue::DownloadQueue(const std::string &apiKey, const std::string &appDir,
                             const std::string &romsDir)
    : apiKey_(apiKey), appDir_(appDir), romsDir_(romsDir) {}

DownloadQueue::~DownloadQueue() {
  cancel();
  if (thread_.joinable()) thread_.join();
}

void DownloadQueue::enqueue(const std::vector<DownloadTask> &tasks) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto &t : tasks) {
    Item it;
    it.task = t;
    items_.push_back(std::move(it));
  }
}

bool DownloadQueue::preflight(uint64_t &needBytes, uint64_t &freeBytes) const {
  uint64_t total = 0;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const auto &it : items_) total += it.task.size;
  }
  struct statvfs v;
  freeBytes = 0;
  if (statvfs(romsDir_.c_str(), &v) == 0)
    freeBytes = (uint64_t)v.f_bavail * (uint64_t)v.f_frsize;
  const uint64_t kReserve = 200ULL * 1024 * 1024;
  needBytes = total + kReserve;
  return freeBytes >= needBytes;
}

void DownloadQueue::start() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) return;
  finished_ = false;
  thread_ = std::thread(&DownloadQueue::worker, this);
}

void DownloadQueue::pause() { paused_ = true; }
void DownloadQueue::resume() { paused_ = false; }

void DownloadQueue::cancel() {
  cancel_ = true;
  paused_ = false;
}

bool DownloadQueue::finished() const { return finished_.load(); }

int DownloadQueue::lastHttpCode() const { return lastHttpCode_.load(); }

std::vector<TaskView> DownloadQueue::snapshot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<TaskView> out;
  out.reserve(items_.size());
  for (const auto &it : items_) {
    TaskView v;
    v.label = it.task.label;
    v.size = it.task.size;
    v.done = it.done;
    v.speed = it.speed;
    v.state = it.state;
    v.error = it.error;
    out.push_back(std::move(v));
  }
  return out;
}

uint64_t DownloadQueue::totalBytes() const {
  std::lock_guard<std::mutex> lk(mutex_);
  uint64_t t = 0;
  for (const auto &it : items_) t += it.task.size;
  return t;
}

uint64_t DownloadQueue::doneBytes() const {
  std::lock_guard<std::mutex> lk(mutex_);
  uint64_t t = 0;
  for (const auto &it : items_) t += it.done;
  return t;
}

void DownloadQueue::worker() {
  DriveClient client(apiKey_, appDir_);
  for (size_t idx = 0; idx < items_.size(); ++idx) {
    if (cancel_.load()) {
      std::lock_guard<std::mutex> lk(mutex_);
      for (size_t j = idx; j < items_.size(); ++j)
        if (items_[j].state == TaskState::Queued)
          items_[j].state = TaskState::Cancelled;
      break;
    }
    {
      std::lock_guard<std::mutex> lk(mutex_);
      items_[idx].state = TaskState::Downloading;
    }
    // Seed progress from an existing .part (resume).
    {
      struct stat st;
      uint64_t partSize = 0;
      if (stat((items_[idx].task.destPath + ".part").c_str(), &st) == 0 &&
          st.st_size > 0)
        partSize = (uint64_t)st.st_size;
      std::lock_guard<std::mutex> lk(mutex_);
      items_[idx].done = partSize;
    }
    auto lastT = std::chrono::steady_clock::now();
    uint64_t lastBytes = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      lastBytes = items_[idx].done;
    }
    DriveError err;
    const bool ok = client.download(
        items_[idx].task.fileId, items_[idx].task.destPath,
        items_[idx].task.md5,
        [&](uint64_t done, uint64_t total) {
          // pause gate: stall the transfer while paused
          while (paused_.load() && !cancel_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (cancel_.load()) return false;
          const auto now = std::chrono::steady_clock::now();
          const double dt =
              std::chrono::duration<double>(now - lastT).count();
          std::lock_guard<std::mutex> lk(mutex_);
          if (dt > 0.25 && done >= lastBytes) {
            const double inst = (done - lastBytes) / dt;
            items_[idx].speed = items_[idx].speed * 0.7 + inst * 0.3;
            lastT = now;
            lastBytes = done;
          }
          items_[idx].done = done;
          items_[idx].state =
              paused_.load() ? TaskState::Paused : TaskState::Downloading;
          if (total > 0 && items_[idx].task.size == 0) {
            // remote size was unknown: adopt it for the progress bar
            items_[idx].task.size = total;
          }
          return true;
        },
        cancel_, err);
    if (err.httpCode != 0) lastHttpCode_ = err.httpCode;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (ok) {
        items_[idx].state = TaskState::Done;
      } else if (cancel_.load()) {
        items_[idx].state = TaskState::Cancelled;
      } else {
        items_[idx].state = TaskState::Error;
        items_[idx].error = err.message;
      }
    }
  }
  finished_ = true;
}

}  // namespace GameSync
