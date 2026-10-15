#pragma once

// Scanner — P2: local game library scan.
// Walks <root> (default /mnt/SDCARD/Roms, override via GS_ROMS_DIR env).
// system code = top-level directory name; files directly under root get "".
// Skips hidden entries (name starts with '.'). Never follows symlinks.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace GameSync {

struct GameFile {
  std::string system;   // top-level dir name, "" = uncategorized
  std::string relPath;  // relative to scan root, e.g. "GBA/game.zip"
  std::string name;     // file name only
  uint64_t size = 0;
  uint64_t mtime = 0;
};

struct ScanResult {
  std::vector<GameFile> files;  // sorted by (system, relPath)
  uint64_t totalBytes = 0;
};

std::string defaultRomsDir();

// Recursive scan. progress counts files found (for UI progress display).
// cancel is checked periodically; scan stops early when set.
void scanRoms(const std::string &root, ScanResult &out,
              std::atomic<uint64_t> &progress, const std::atomic<bool> &cancel);

// Binary cache with version header. Returns false on any format mismatch.
bool saveScanCache(const std::string &path, const ScanResult &res);
bool loadScanCache(const std::string &path, ScanResult &res);

}  // namespace GameSync
