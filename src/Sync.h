#pragma once

// Sync — P4: compare local library vs Drive, download queue.
// v1 NEVER deletes local files: LOCAL_ONLY rows are display-only.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Drive.h"
#include "Scanner.h"

namespace GameSync {

enum class DiffStatus { SYNCED, REMOTE_ONLY, LOCAL_ONLY, DIFFER };

// Short Vietnamese label for UI rows.
const char *diffStatusLabel(DiffStatus s);

struct DiffEntry {
  std::string system;   // Roms code, "" = uncategorized
  std::string relPath;  // path within system (no Drive-folder / no system prefix)
  std::string name;     // file name, or group stem for multi-disc groups
  uint64_t localSize = 0;
  // Bytes that would be downloaded for this row (0 for SYNCED / LOCAL_ONLY).
  uint64_t remoteSize = 0;
  std::string remoteId;   // empty when no remote counterpart
  std::string remoteMd5;  // may be empty
  DiffStatus status = DiffStatus::SYNCED;
  bool isGroup = false;
  std::vector<DiffEntry> members;  // when isGroup
};

// Join local vs remote on (system, path-within-system).
// Remote: first component of Drive relPath (the Drive folder name) is stripped.
// Multi-disc PS files (.cue/.bin/.m3u/.pbp sharing one stem in one dir) are
// merged into a single group row. Result sorted by (system, relPath).
std::vector<DiffEntry> compareLibrary(const ScanResult &local,
                                      const std::vector<DriveFile> &remote);

// Destination path safety: returns "" when unsafe (path traversal, absolute
// path, bad system code, empty name). Fail closed — caller must skip "".
std::string safeRomsPath(const std::string &romsDir, const std::string &system,
                         const std::string &relInSys);

struct DownloadTask {
  std::string label;     // display name
  std::string fileId;    // Drive file id
  std::string destPath;  // final path inside Roms/ (sanitized)
  uint64_t size = 0;     // remote size (0 = unknown)
  std::string md5;       // expected md5 (may be empty)
};

// Expand marked diff rows into download tasks (groups expand to their
// downloadable members). skippedUnsafe counts rows dropped by safeRomsPath.
std::vector<DownloadTask> buildDownloadTasks(const std::vector<DiffEntry> &rows,
                                            const std::string &romsDir,
                                            int &skippedUnsafe);

enum class TaskState { Queued, Downloading, Paused, Done, Error, Cancelled };

struct TaskView {
  std::string label;
  uint64_t size = 0;
  uint64_t done = 0;
  double speed = 0;  // bytes/sec, smoothed
  TaskState state = TaskState::Queued;
  std::string error;
};

// Background download queue. One worker thread, sequential downloads.
class DownloadQueue {
 public:
  DownloadQueue(const std::string &apiKey, const std::string &appDir,
                const std::string &romsDir);
  ~DownloadQueue();

  void enqueue(const std::vector<DownloadTask> &tasks);
  // Pre-flight: total selected size vs free space minus 200 MB reserve.
  // Returns true when there is room; else fills needBytes/freeBytes.
  bool preflight(uint64_t &needBytes, uint64_t &freeBytes) const;
  void start();    // spawn worker (no-op if already started)
  void pause();    // worker stalls between chunks until resume()
  void resume();
  void cancel();   // stop after current file; remaining -> Cancelled
  bool finished() const;
  bool paused() const { return paused_.load(); }
  int lastHttpCode() const;  // last HTTP code seen (401/403 => bad API key)

  std::vector<TaskView> snapshot() const;
  uint64_t totalBytes() const;
  uint64_t doneBytes() const;

 private:
  void worker();

  struct Item {
    DownloadTask task;
    TaskState state = TaskState::Queued;
    uint64_t done = 0;
    double speed = 0;
    std::string error;
  };

  std::string apiKey_;
  std::string appDir_;
  std::string romsDir_;
  mutable std::mutex mutex_;
  std::vector<Item> items_;
  std::thread thread_;
  std::atomic<bool> started_{false};
  std::atomic<bool> paused_{false};
  std::atomic<bool> cancel_{false};
  std::atomic<bool> finished_{false};
  std::atomic<int> lastHttpCode_{0};
};

}  // namespace GameSync
