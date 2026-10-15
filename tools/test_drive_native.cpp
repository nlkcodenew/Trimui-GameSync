// test_drive_native.cpp — Native (host g++) test for the C++ Drive module.
// Compiles Drive.cpp natively with a stub curl header; unreferenced
// curl-dependent functions are dropped via -ffunction-sections -Wl,--gc-sections.
// Covers: parseDriveFolderId (same cases as the Python replica), cache
// roundtrip, and reading the .bin produced by drive_list_replica.py against
// the mock server (byte-format cross-check C++ <-> Python).
#include "Drive.h"

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

void testParse() {
  struct T {
    const char *in;
    const char *want;
  };
  const T tests[] = {
      {"https://drive.google.com/drive/folders/1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
       "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"},
      {"https://drive.google.com/drive/u/0/folders/"
       "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
       "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"},
      {"https://drive.google.com/open?id=1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
       "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"},
      {"https://drive.google.com/drive/folders/1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"
       "?usp=sharing",
       "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"},
      {"https://drive.google.com/drive/folders/1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM#list",
       "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"},
      {"1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM",
       "1b67C2hqP5amYX3ZmAGCYd_7PeY5HoenM"},
      {"https://drive.google.com/drive/folders/ABC_def-123", "ABC_def-123"},
      {"not a link!!!", ""},
      {"", ""},
      {"https://example.com/", ""},
      {"https://drive.google.com/drive/folders/short", ""},
  };
  for (const auto &t : tests) {
    const std::string got = parseDriveFolderId(t.in);
    if (got != t.want) {
      ++failures;
      std::cout << "FAIL parse: " << t.in << " -> '" << got << "' want '"
                << t.want << "'\n";
    }
  }
  std::cout << "parse: " << (sizeof(tests) / sizeof(tests[0]))
            << " cases done\n";
}

void testCacheRoundtrip() {
  std::vector<DriveFile> files;
  DriveFile a;
  a.relPath = "Nintendo - Game Boy Advance/game1.zip";
  a.driveFolder = "Nintendo - Game Boy Advance";
  a.system = "GBA";
  a.id = "g1";
  a.size = 1234567;
  a.md5 = "d41d8cd98f00b204e9800998ecf8427e";
  a.mtime = "2026-01-02T03:04:05.000Z";
  DriveFile b;
  b.relPath = "Pok\xC3\xA9mon \xE2\x80\x94 Red (USA).zip";  // unicode name
  b.driveFolder = "";
  b.system = "";
  b.id = "doc2";
  b.size = 100;
  files.push_back(a);
  files.push_back(b);
  const std::string path = "/tmp/gsync/rt.bin";
  CHECK(saveDriveCache(path, files));
  std::vector<DriveFile> back;
  CHECK(loadDriveCache(path, back));
  CHECK(back.size() == 2);
  if (back.size() == 2) {
    CHECK(back[0].relPath == a.relPath);
    CHECK(back[0].driveFolder == a.driveFolder);
    CHECK(back[0].system == a.system);
    CHECK(back[0].id == a.id);
    CHECK(back[0].size == a.size);
    CHECK(back[0].md5 == a.md5);
    CHECK(back[0].mtime == a.mtime);
    CHECK(back[1].relPath == b.relPath);
    CHECK(back[1].size == b.size);
  }
  uint64_t n = 0;
  CHECK(driveCacheFileCount(path, n) && n == 2);
  // corrupt magic -> must fail
  FILE *f = fopen(path.c_str(), "r+b");
  if (f) {
    fputc('X', f);
    fclose(f);
    std::vector<DriveFile> bad;
    CHECK(!loadDriveCache(path, bad));
  }
  std::cout << "cache roundtrip done\n";
}

// Reads the .bin written by drive_list_replica.py against mock_drive_server.py
// and asserts the exact expected listing (PortMaster skipped, sorted, unicode ok).
void testPythonBin(const char *path) {
  std::vector<DriveFile> files;
  CHECK(loadDriveCache(path, files));
  CHECK(files.size() == 5);
  if (files.size() != 5) {
    std::cout << "got " << files.size() << " files\n";
    return;
  }
  // Expected order: byte-wise UTF-8 sort by relPath.
  CHECK(files[0].relPath == "Nintendo - Game Boy Advance/Sub/nested.bin");
  CHECK(files[0].driveFolder == "Nintendo - Game Boy Advance");
  CHECK(files[0].system == "GBA");
  CHECK(files[0].md5 == "");  // no md5Checksum field in mock
  CHECK(files[1].relPath == "Nintendo - Game Boy Advance/game1.zip");
  CHECK(files[1].system == "GBA");
  CHECK(files[1].size == 1234567);
  CHECK(files[1].md5 == "d41d8cd98f00b204e9800998ecf8427e");
  CHECK(files[2].relPath == "Pok\xC3\xA9mon \xE2\x80\x94 Red (USA).zip");
  CHECK(files[2].driveFolder == "");
  CHECK(files[2].system == "");
  CHECK(files[3].relPath == "Sega - Genesis/Weird \"quoted\" (name).md");
  CHECK(files[3].system == "MD");
  CHECK(files[4].relPath == "readme.txt");
  CHECK(files[4].size == 42);
  for (const auto &df : files) {
    CHECK(df.relPath.find("PortMaster") == std::string::npos);
    CHECK(df.relPath.find("should_not_appear") == std::string::npos);
  }
  std::cout << "python .bin cross-check done\n";
}

int main(int argc, char **argv) {
  testParse();
  testCacheRoundtrip();
  if (argc > 1) testPythonBin(argv[1]);
  if (failures == 0) {
    std::cout << "ALL NATIVE TESTS PASSED\n";
    return 0;
  }
  std::cout << failures << " FAILURES\n";
  return 1;
}
