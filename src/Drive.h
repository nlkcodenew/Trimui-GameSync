#pragma once

// Drive — P3: Google Drive API v3 client (API key, no OAuth).
// Lists shared folders recursively, caches results locally.
// The API key is NEVER hardcoded: resolveDriveApiKey() reads env
// GS_DRIVE_API_KEY first, then the "drive_api_key" setting in data/.

#include <atomic>
#include <functional>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace GameSync {

// Extract a Drive folder ID from any common Drive URL shape, or a raw ID.
// Returns "" when nothing usable is found.
std::string parseDriveFolderId(std::string s);

// API key resolution: env GS_DRIVE_API_KEY > data/settings "drive_api_key".
std::string resolveDriveApiKey();

struct DriveItem {
  std::string id;
  std::string name;
  std::string mimeType;
  uint64_t size = 0;  // 0 when absent (folders)
  std::string md5;    // md5Checksum, may be empty
  std::string mtime;  // modifiedTime (RFC3339), may be empty
  bool isFolder = false;
};

struct DriveError {
  int httpCode = 0;  // 0 = transport-level failure
  std::string message;
};

struct DriveFile {
  std::string relPath;     // relative to source root, e.g.
                           // "Nintendo - Game Boy Advance/game.zip"
  std::string driveFolder;  // top-level Drive folder name ("" for root files)
  std::string system;      // mapped Roms/ code ("" when unmapped)
  std::string id;          // Drive file id
  uint64_t size = 0;
  std::string md5;
  std::string mtime;
};

class DriveClient {
 public:
  // appDir is used to locate certs/cacert.pem for HTTPS.
  DriveClient(const std::string &apiKey, const std::string &appDir);

  // List direct children of a folder (pageToken pagination handled inside).
  // Returns false and fills err on any HTTP/API/parse failure.
  bool listChildren(const std::string &folderId, std::vector<DriveItem> &out,
                    DriveError &err);

  // Recursive listing of every file under rootFolderId.
  // - Skips any folder named exactly "PortMaster" (out of app scope).
  // - driveFolder = first-level folder name; system via systemMap
  //   (Drive folder name -> Roms code); "" when unmapped.
  // - progress counts files found; cancel stops ASAP.
  // - out is sorted by relPath on success.
  bool listRecursive(const std::string &rootFolderId,
                     const std::map<std::string, std::string> &systemMap,
                     std::vector<DriveFile> &out,
                     std::atomic<uint64_t> &progress,
                     const std::atomic<bool> &cancel, DriveError &err);

  // Download a Drive file (alt=media) to destPath via destPath+.part,
  // then rename. Resumes from an existing .part (HTTP Range).
  // progress(doneBytes, totalBytes) -> bool: return false to abort.
  // cancel aborts ASAP (.part kept for resume). Verifies expectedMd5
  // (when non-empty) with EVP MD5; mismatch deletes the .part.
  // Returns false + err on failure.
  bool download(const std::string &fileId, const std::string &destPath,
                const std::string &expectedMd5,
                const std::function<bool(uint64_t, uint64_t)> &progress,
                const std::atomic<bool> &cancel, DriveError &err);

 private:
  // API base URL; GS_DRIVE_BASE_URL env overrides it (used by native tests
  // against tools/mock_drive_dl.py). Production default is googleapis.
  std::string apiBase() const;
  bool downloadOnce(const std::string &fileId, const std::string &destPath,
                    const std::string &expectedMd5, uint64_t offset,
                    const std::function<bool(uint64_t, uint64_t)> &progress,
                    const std::atomic<bool> &cancel, DriveError &err,
                    bool &serverIgnoredRange);

 private:
  bool httpGet(const std::string &url, std::string &body, long &httpCode,
               std::string &curlErr);
  static std::string urlEncode(const std::string &s);

  std::string apiKey_;
  std::string caBundle_;
};

// Load {"mapping": {driveName: romsCode, ...}} from system_map.json.
bool loadSystemMap(const std::string &path,
                   std::map<std::string, std::string> &out);

// Binary cache: magic "GSDRV001" (8 bytes), u64 file count, then per file:
//   u32 len + bytes (relPath), u32 len + bytes (driveFolder),
//   u32 len + bytes (system), u32 len + bytes (id),
//   u64 size, u32 len + bytes (md5), u32 len + bytes (mtime).
// All integers little-endian. Returns false on any format mismatch.
bool saveDriveCache(const std::string &path,
                    const std::vector<DriveFile> &files);
bool loadDriveCache(const std::string &path, std::vector<DriveFile> &files);
// Read only the header (file count) without loading everything.
bool driveCacheFileCount(const std::string &path, uint64_t &count);

// Cache file path for one source (folderId is [A-Za-z0-9_-], fs-safe).
std::string driveCachePath(const std::string &dataDir,
                           const std::string &folderId);

}  // namespace GameSync
