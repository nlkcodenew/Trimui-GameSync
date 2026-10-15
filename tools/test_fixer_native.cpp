// test_fixer_native.cpp — Native (host g++) test for the C++ Fixer module.
// Compiles Fixer.cpp + Scanner.cpp natively (no curl needed).
// Usage:
//   ./test_fixer_native                       # unit tests only
//   ./test_fixer_native scan_cache.bin        # unit tests + canonical summary
// The summary format must match tools/fixer_replica.py byte for byte:
//   COUNTS MOVE=<n> WARN=<n> GROUPS=<n> FILES=<n>
//   M|<system>|<relPath>|<expected>|<size>
//   G|<system>|<relDir>|<stem>|<expected>|<members>|<size>
//   W|<system>|<relPath>|<ext>|<note>
#include "Fixer.h"
#include "Scanner.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

using namespace GameSync;

static int failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      ++failures;                                                       \
      std::cout << "FAIL line " << __LINE__ << ": " #cond "\n";          \
    }                                                                   \
  } while (0)

static GameFile mk(const char *sys, const char *rel, uint64_t size = 100) {
  GameFile f;
  f.system = sys;
  f.relPath = rel;
  const std::string r = rel;
  const size_t p = r.rfind('/');
  f.name = (p == std::string::npos) ? r : r.substr(p + 1);
  f.size = size;
  return f;
}

static const FixEntry *findByRel(const std::vector<FixEntry> &v,
                                 const std::string &rel) {
  for (const auto &e : v)
    if (e.relPath == rel) return &e;
  return nullptr;
}

void testExtMap() {
  CHECK(std::string(expectedSystemForExt(".nes")) == "FC");
  CHECK(std::string(expectedSystemForExt(".NES")) == "");  // not lowered
  CHECK(std::string(expectedSystemForExt(".fds")) == "FC");
  CHECK(std::string(expectedSystemForExt(".sfc")) == "SFC");
  CHECK(std::string(expectedSystemForExt(".smc")) == "SFC");
  CHECK(std::string(expectedSystemForExt(".gb")) == "GB");
  CHECK(std::string(expectedSystemForExt(".gbc")) == "GBC");
  CHECK(std::string(expectedSystemForExt(".gba")) == "GBA");
  CHECK(std::string(expectedSystemForExt(".md")) == "MD");
  CHECK(std::string(expectedSystemForExt(".gen")) == "MD");
  CHECK(std::string(expectedSystemForExt(".smd")) == "MD");
  CHECK(std::string(expectedSystemForExt(".sms")) == "MS");
  CHECK(std::string(expectedSystemForExt(".gg")) == "GG");
  CHECK(std::string(expectedSystemForExt(".pce")) == "PCE");
  CHECK(std::string(expectedSystemForExt(".a26")) == "ATARI2600");
  CHECK(std::string(expectedSystemForExt(".nds")) == "NDS");
  CHECK(std::string(expectedSystemForExt(".cue")) == "PS");
  CHECK(std::string(expectedSystemForExt(".bin")) == "PS");
  CHECK(std::string(expectedSystemForExt(".m3u")) == "PS");
  CHECK(std::string(expectedSystemForExt(".pbp")) == "PS");
  CHECK(std::string(expectedSystemForExt(".iso")) == "");
  CHECK(std::string(expectedSystemForExt(".zip")) == "");
  CHECK(std::string(expectedSystemForExt(".txt")) == "");
  CHECK(isAmbiguousExt(".iso") && isAmbiguousExt(".cso") &&
        isAmbiguousExt(".chd") && isAmbiguousExt(".zip") &&
        isAmbiguousExt(".7z") && isAmbiguousExt(".rar"));
  CHECK(!isAmbiguousExt(".nes") && !isAmbiguousExt(""));
  CHECK(fileExtLower("Game.NES") == ".nes");
  CHECK(fileExtLower("noext") == "");
  CHECK(fileExtLower(".hidden") == ".hidden");  // scanner skips dotfiles anyway
  CHECK(isCompatiblePlacement("GB", "GBC"));
  CHECK(!isCompatiblePlacement("GBC", "GB"));
  CHECK(!isCompatiblePlacement("GB", "GB"));
}

void testFindMisplaced() {
  ScanResult scan;
  scan.files = {
      mk("SFC", "SFC/zelda.nes"),          // move -> FC
      mk("SFC", "SFC/mario.sfc"),          // ok
      mk("GBC", "GBC/tetris.gb"),         // compatible -> warn only
      mk("GB", "GB/pokemon.gbc"),         // move -> GBC
      mk("PS", "PS/game.iso"),            // ambiguous -> warn
      mk("MD", "MD/doom.zip"),            // archive -> warn
      mk("GBA", "GBA/readme"),            // no ext -> warn
      mk("FC", "FC/notes.txt"),           // unknown ext -> skipped
      mk("SFC", "SFC/disc.cue"),          // group with disc.bin -> PS
      mk("SFC", "SFC/disc.bin", 200),     //
      mk("MD", "MD/sonic.MD"),            // ok (uppercase ext)
      mk("", "stray.nes"),                // root of Roms -> move FC
      mk("GG", "GG/sub/sonic.gg"),        // ok, subdir
      mk("MS", "MS/deep/a/b/alex.sms"),   // ok, deep subdir
      mk("NES", "NES/odd.gba"),           // move -> GBA (folder NES unknown)
      mk("ports", "ports/dustaet/dustaet.md"),  // port data -> skipped
      mk("A30PORTS", "A30PORTS/x/assets.bin"),  // port data -> skipped
  };
  const std::vector<FixEntry> v = findMisplaced(scan);
  // Expected: moves = zelda.nes, pokemon.gbc, disc-group, stray.nes, odd.gba
  // (5 entries, 1 group); warns = tetris.gb, game.iso, doom.zip, readme (4).
  int nMove = 0, nWarn = 0, nGroup = 0;
  for (const auto &e : v) {
    if (e.kind == FixKind::Move) {
      ++nMove;
      if (e.isGroup) ++nGroup;
    } else {
      ++nWarn;
    }
  }
  CHECK(nMove == 5);
  CHECK(nWarn == 4);
  CHECK(nGroup == 1);
  CHECK(v.size() == 9);

  const FixEntry *z = findByRel(v, "SFC/zelda.nes");
  CHECK(z && z->kind == FixKind::Move && z->expected == "FC");
  const FixEntry *t = findByRel(v, "GBC/tetris.gb");
  CHECK(t && t->kind == FixKind::WarnOnly);
  const FixEntry *p = findByRel(v, "GB/pokemon.gbc");
  CHECK(p && p->kind == FixKind::Move && p->expected == "GBC");
  const FixEntry *g = findByRel(v, "SFC/disc");
  CHECK(g && g->isGroup && g->members.size() == 2 && g->expected == "PS" &&
        g->size == 300);
  const FixEntry *s = findByRel(v, "stray.nes");
  CHECK(s && s->kind == FixKind::Move && s->expected == "FC");
  // sorted by (system, relPath): "" < "GB" < "GBC" < "NES" < "SFC"
  CHECK(v.front().system == "");
  CHECK(findByRel(v, "FC/notes.txt") == nullptr);  // unknown ext skipped
  CHECK(findByRel(v, "SFC/mario.sfc") == nullptr);
}

static void sh(const char *cmd) {
  const int rc = system(cmd);
  (void)rc;
}

static void writeFile(const std::string &p, const std::string &data) {
  std::ofstream f(p, std::ios::binary);
  f << data;
}

void testApplyFixes() {
  const std::string base = "/tmp/fixer_apply_test";
  struct stat st{};
  sh("rm -rf /tmp/fixer_apply_test");
  sh("mkdir -p /tmp/fixer_apply_test/Roms/SFC /tmp/fixer_apply_test/Roms/FC /tmp/fixer_apply_test/Roms/MD/sub /tmp/fixer_apply_test/app/data");
  const std::string roms = base + "/Roms";
  const std::string app = base + "/app";
  writeFile(roms + "/SFC/game.nes", "nesdata");
  writeFile(roms + "/SFC/disc.cue", "cue");
  writeFile(roms + "/SFC/disc.bin", "bindata");
  writeFile(roms + "/FC/game.nes", "already-here");  // dest exists -> skip
  writeFile(roms + "/MD/sub/sonic.sfc", "sfcdata");   // subdir preserved on move

  // Build scan via the real scanner.
  std::atomic<uint64_t> prog{0};
  const std::atomic<bool> cancel{false};
  ScanResult scan;
  scanRoms(roms, scan, prog, cancel);
  CHECK(scan.files.size() == 5);

  // Move the two single files: game.nes skipped (dest exists), sonic.sfc moved
  // with its subdir preserved.
  std::vector<FixEntry> v = findMisplaced(scan);
  std::vector<FixEntry> one;
  for (const auto &e : v)
    if (!e.isGroup && e.kind == FixKind::Move) one.push_back(e);
  CHECK(one.size() == 2);
  FixResult r1 = applyFixes(one, roms, app);
  CHECK(r1.moved == 1 && r1.skipped == 1);
  CHECK(stat((roms + "/SFC/sub/sonic.sfc").c_str(), &st) == 0);

  // Now move the group -> PS/.
  std::vector<FixEntry> grp;
  for (const auto &e : v)
    if (e.isGroup) grp.push_back(e);
  CHECK(grp.size() == 1);
  FixResult r2 = applyFixes(grp, roms, app);
  CHECK(r2.moved == 2 && r2.skipped == 0);
  CHECK(stat((roms + "/PS/disc.cue").c_str(), &st) == 0);
  CHECK(stat((roms + "/PS/disc.bin").c_str(), &st) == 0);
  CHECK(stat((roms + "/SFC/disc.cue").c_str(), &st) != 0);
  // fix.log written with 3 lines (1 single + 2 group members).
  std::ifstream log(app + "/data/fix.log");
  int lines = 0;
  std::string l;
  bool hasArrow = false;
  while (std::getline(log, l)) {
    ++lines;
    if (l.find(" -> ") != std::string::npos) hasArrow = true;
  }
  CHECK(lines == 3 && hasArrow);
  sh("rm -rf /tmp/fixer_apply_test");
}

int main(int argc, char **argv) {
  testExtMap();
  testFindMisplaced();
  testApplyFixes();
  if (argc > 1) {
    // Summary mode: stdout must match tools/fixer_replica.py byte for byte,
    // so no test banner here (failures still surface via exit code).
    ScanResult scan;
    if (!loadScanCache(argv[1], scan)) {
      std::cout << "FAIL: cannot load scan cache " << argv[1] << "\n";
      return 1;
    }
    std::cout << fixSummary(findMisplaced(scan));
    return failures ? 1 : 0;
  }
  if (failures == 0) std::cout << "ALL UNIT TESTS PASSED\n";
  return failures ? 1 : 0;
}
