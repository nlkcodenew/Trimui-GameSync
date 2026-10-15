// test_sync_native.cpp — Native (host g++) test for the C++ Sync module.
// Compiles Sync.cpp + Scanner.cpp natively; DriveClient methods used only by
// the download queue are stubbed (the queue worker never runs here).
// Usage:
//   ./test_sync_native                       # unit tests only
//   ./test_sync_native scan.bin drive1.bin [drive2.bin ...]
//                                            # unit tests + canonical summary
// The summary format must match tools/sync_compare_replica.py byte for byte:
//   COUNTS SYNCED=<n> REMOTE_ONLY=<n> LOCAL_ONLY=<n> DIFFER=<n> ROWS=<n>
//   F|STATUS|system|relPath|localSize|remoteSize
//   G|STATUS|system|relPath|nMembers|localSize|remoteSize
//   M|STATUS|system|relPath|localSize|remoteSize
#include "Sync.h"

#include <cassert>
#include <cstdio>
#include <iostream>

using namespace GameSync;


static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      std::cout << "FAIL line " << __LINE__ << ": " #cond "\n";          \
    }                                                                   \
  } while (0)

static const char *statusName(DiffStatus s) {
  switch (s) {
    case DiffStatus::SYNCED: return "SYNCED";
    case DiffStatus::REMOTE_ONLY: return "REMOTE_ONLY";
    case DiffStatus::LOCAL_ONLY: return "LOCAL_ONLY";
    case DiffStatus::DIFFER: return "DIFFER";
  }
  return "?";
}

static GameFile mkLocal(const char *sys, const char *rel, uint64_t size) {
  GameFile f;
  f.system = sys;
  f.relPath = rel;
  f.name = rel;
  f.size = size;
  return f;
}

static DriveFile mkRemote(const char *sys, const char *rel, const char *id,
                          uint64_t size) {
  DriveFile f;
  f.relPath = rel;
  f.system = sys;
  f.id = id;
  f.size = size;
  return f;
}

void testSanitize() {
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GBA", "game.zip") ==
        "/mnt/SDCARD/Roms/GBA/game.zip");
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GBA", "sub/dir/game.zip") ==
        "/mnt/SDCARD/Roms/GBA/sub/dir/game.zip");
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GBA", "../evil.zip").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GBA", "sub/../../evil.zip").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GBA", "/abs/path.zip").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GBA", "").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GBA", "..").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "../X", "game.zip").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "GB/A", "game.zip").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "", "game.zip").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms", "PS", "a\\b.zip").empty());
  CHECK(safeRomsPath("/mnt/SDCARD/Roms/", "PS", "./x.bin") ==
        "/mnt/SDCARD/Roms/PS/x.bin");
  // buildDownloadTasks skips unsafe + LOCAL_ONLY/SYNCED rows
  DiffEntry bad;
  bad.system = "GBA";
  bad.relPath = "../evil.zip";
  bad.name = "evil.zip";
  bad.status = DiffStatus::REMOTE_ONLY;
  bad.remoteId = "id1";
  bad.remoteSize = 10;
  DiffEntry good;
  good.system = "GBA";
  good.relPath = "ok.zip";
  good.name = "ok.zip";
  good.status = DiffStatus::DIFFER;
  good.remoteId = "id2";
  good.remoteSize = 20;
  DiffEntry localOnly;
  localOnly.system = "GBA";
  localOnly.relPath = "mine.zip";
  localOnly.name = "mine.zip";
  localOnly.status = DiffStatus::LOCAL_ONLY;
  int skipped = 0;
  std::vector<DiffEntry> rows = {bad, good, localOnly};
  auto tasks = buildDownloadTasks(rows, "/mnt/SDCARD/Roms", skipped);
  CHECK(skipped == 1);
  CHECK(tasks.size() == 1);
  CHECK(tasks[0].destPath == "/mnt/SDCARD/Roms/GBA/ok.zip");
  CHECK(tasks[0].fileId == "id2");
}

void testCompare() {
  ScanResult local;
  local.files = {mkLocal("GBA", "GBA/a.zip", 100), mkLocal("GBA", "GBA/b.zip", 100),
                 mkLocal("GBA", "GBA/c.zip", 100)};
  std::vector<DriveFile> remote = {
      mkRemote("GBA", "Nintendo - Game Boy Advance/a.zip", "id1", 100),
      mkRemote("GBA", "Nintendo - Game Boy Advance/b.zip", "id2", 200),
      mkRemote("GBA", "Nintendo - Game Boy Advance/e.zip", "id3", 300)};
  auto rows = compareLibrary(local, remote);
  std::map<std::string, DiffStatus> got;
  for (const auto &e : rows) got[e.relPath] = e.status;
  CHECK(got["a.zip"] == DiffStatus::SYNCED);
  CHECK(got["b.zip"] == DiffStatus::DIFFER);
  CHECK(got["c.zip"] == DiffStatus::LOCAL_ONLY);
  CHECK(got["e.zip"] == DiffStatus::REMOTE_ONLY);

  // multi-disc: mixed -> DIFFER group, remoteSize = downloadable only
  ScanResult l2;
  l2.files = {mkLocal("PS", "PS/g.cue", 10)};
  std::vector<DriveFile> r2 = {mkRemote("PS", "Sony - PS1/g.cue", "i1", 10),
                               mkRemote("PS", "Sony - PS1/g.bin", "i2", 500)};
  auto g = compareLibrary(l2, r2);
  CHECK(g.size() == 1 && g[0].isGroup);
  CHECK(g[0].status == DiffStatus::DIFFER);
  CHECK(g[0].remoteSize == 500);
  CHECK(g[0].members.size() == 2);

  // all-synced group
  ScanResult l3;
  l3.files = {mkLocal("PS", "PS/h.cue", 10), mkLocal("PS", "PS/h.bin", 500)};
  std::vector<DriveFile> r3 = {mkRemote("PS", "Sony - PS1/h.cue", "i1", 10),
                               mkRemote("PS", "Sony - PS1/h.bin", "i2", 500)};
  auto g3 = compareLibrary(l3, r3);
  CHECK(g3.size() == 1 && g3[0].status == DiffStatus::SYNCED);
  CHECK(g3[0].remoteSize == 0);
}


// ---- P4 download tests against tools/mock_drive_dl.py ----
// Run: test_sync_native --dl-test <base-url> <md5small> <md5big>
// (start the mock first; it prints PORT and MD5 lines)

#include <sys/stat.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace {
bool fileExistsQ(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}
std::string readWhole(const std::string &p) {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}
bool progTrue(uint64_t, uint64_t) { return true; }
}  // namespace

void testDownload(const std::string &base, const std::string &md5small,
                  const std::string &md5big) {
  setenv("GS_DRIVE_BASE_URL", base.c_str(), 1);
  const std::string dir = "/tmp/gsync_dl_test";
  (void)system("rm -rf /tmp/gsync_dl_test");
  mkdir(dir.c_str(), 0777);
  DriveClient c("dummy-key", dir);
  std::atomic<bool> cancel{false};
  DriveError err;

  // 1. full download with correct md5
  {
    const std::string dest = dir + "/PS/small1.bin";
    CHECK(c.download("small1", dest, md5small, progTrue, cancel, err));
    CHECK(readWhole(dest).size() == 2300);
    CHECK(!fileExistsQ(dest + ".part"));
  }
  // 2. wrong md5 -> fail, .part deleted, dest absent
  {
    const std::string dest = dir + "/PS/small1_bad.bin";
    err = DriveError{};
    CHECK(!c.download("small1", dest, "00000000000000000000000000000000",
                      progTrue, cancel, err));
    CHECK(!fileExistsQ(dest + ".part"));
    CHECK(!fileExistsQ(dest));
  }
  // 3. cancel mid-download -> .part kept, partial size
  {
    const std::string dest = dir + "/PS/big1.bin";
    std::atomic<bool> cc{false};
    std::thread killer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
      cc = true;
    });
    err = DriveError{};
    auto progCancel = [&](uint64_t, uint64_t) { return !cc.load(); };
    const bool ok = c.download("big1", dest, md5big, progCancel, cc, err);
    killer.join();
    CHECK(!ok);
    struct stat st;
    CHECK(stat((dest + ".part").c_str(), &st) == 0);
    CHECK(st.st_size > 0 && (uint64_t)st.st_size < 8ULL * 1024 * 1024);
  }
  // 4. resume the partial file -> completes with md5 ok
  {
    const std::string dest = dir + "/PS/big1.bin";
    err = DriveError{};
    CHECK(c.download("big1", dest, md5big, progTrue, cancel, err));
    CHECK(readWhole(dest).size() == 8ULL * 1024 * 1024);
    CHECK(!fileExistsQ(dest + ".part"));
  }
  // 5. unknown id -> 404
  {
    err = DriveError{};
    CHECK(
        !c.download("nope", dir + "/PS/nope.bin", "", progTrue, cancel, err));
    CHECK(err.httpCode == 404);
  }
  std::cerr << "DOWNLOAD TESTS DONE\n";
}

void printSummary(const std::vector<DiffEntry> &rows) {
  int nS = 0, nR = 0, nL = 0, nD = 0;
  for (const auto &e : rows) {
    switch (e.status) {
      case DiffStatus::SYNCED: ++nS; break;
      case DiffStatus::REMOTE_ONLY: ++nR; break;
      case DiffStatus::LOCAL_ONLY: ++nL; break;
      case DiffStatus::DIFFER: ++nD; break;
    }
  }
  printf("COUNTS SYNCED=%d REMOTE_ONLY=%d LOCAL_ONLY=%d DIFFER=%d ROWS=%d\n",
         nS, nR, nL, nD, (int)rows.size());
  for (const auto &e : rows) {
    if (e.isGroup) {
      printf("G|%s|%s|%s|%d|%llu|%llu\n", statusName(e.status),
             e.system.c_str(), e.relPath.c_str(), (int)e.members.size(),
             (unsigned long long)e.localSize, (unsigned long long)e.remoteSize);
      for (const auto &m : e.members)
        printf("M|%s|%s|%s|%llu|%llu\n", statusName(m.status),
               m.system.c_str(), m.relPath.c_str(),
               (unsigned long long)m.localSize,
               (unsigned long long)m.remoteSize);
    } else {
      printf("F|%s|%s|%s|%llu|%llu\n", statusName(e.status), e.system.c_str(),
             e.relPath.c_str(), (unsigned long long)e.localSize,
             (unsigned long long)e.remoteSize);
    }
  }
}

int main(int argc, char **argv) {
  testSanitize();
  testCompare();
  if (failures == 0) {
    std::cerr << "ALL NATIVE UNIT TESTS PASSED\n";
  } else {
    std::cerr << failures << " FAILURES\n";
    return 1;
  }
  if (argc > 3 && std::string(argv[1]) == "--dl-test") {
    testDownload(argv[2], argv[3], argv[4]);
    return failures ? 1 : 0;
  }
  if (argc > 2) {
    ScanResult local;
    if (!loadScanCache(argv[1], local)) {
      std::cout << "cannot load scan cache\n";
      return 2;
    }
    std::vector<DriveFile> remote;
    for (int i = 2; i < argc; ++i) {
      std::vector<DriveFile> f;
      if (!loadDriveCache(argv[i], f)) {
        std::cout << "cannot load drive cache " << argv[i] << "\n";
        return 2;
      }
      remote.insert(remote.end(), f.begin(), f.end());
    }
    printSummary(compareLibrary(local, remote));
  }
  return 0;
}
