#pragma once

// Fixer — P5: detect game files placed in the wrong Roms/<SYSTEM>/ folder
// and move them (only with user confirmation) to the right one.
// Pure logic: dirent/stat/fstream only, so it builds natively for unit tests.
// v1 never deletes anything: moves are renames on the same SD card.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "Scanner.h"

namespace GameSync {

// Lowercase extension with dot (".nes"); "" when the name has no extension.
std::string fileExtLower(const std::string &name);

// Expected Roms system code for a lowercase extension, "" when the extension
// is unknown or ambiguous (.iso/.cso/.chd/.zip/.7z/.rar -> "", see
// isAmbiguousExt()).
const char *expectedSystemForExt(const std::string &extLower);

// Game-related but ambiguous extensions: reported for manual review,
// never auto-suggested for a move.
bool isAmbiguousExt(const std::string &extLower);

// True when a file of `fileSystem` may stay in folder `folderSystem`
// without a move suggestion (sub-system compatibility, e.g. a .gb file
// inside GBC/ — a Game Boy Color plays Game Boy games).
bool isCompatiblePlacement(const std::string &fileSystem,
                           const std::string &folderSystem);

// Display rows for the Fix screen mapping table, e.g. ".nes / .fds" -> "FC".
// Single source of truth: generated from the same table as the lookup.
std::vector<std::pair<std::string, std::string>> extMapDisplayRows();

enum class FixKind { Move, WarnOnly };

struct FixEntry {
  std::string system;    // folder system code where the file sits now
  std::string relPath;   // path within scan root, e.g. "SFC/sub/game.nes"
  std::string name;      // display name (groups: "stem.* (N files)")
  std::string ext;       // lowercase ext with dot ("" for groups)
  std::string expected;  // target system code ("" for WarnOnly)
  std::string note;      // human note for WarnOnly rows
  FixKind kind = FixKind::Move;
  bool isGroup = false;
  std::vector<FixEntry> members;  // group members (each kind == Move)
  uint64_t size = 0;             // groups: sum of members
};

// Scan -> misplaced/ambiguous files, sorted by (system, relPath).
// Multi-disc sets (.cue/.bin/.m3u/.pbp sharing one stem in one dir) become
// a single grouped entry so they always move together.
std::vector<FixEntry> findMisplaced(const ScanResult &scan);

struct FixResult {
  int moved = 0;
  int skipped = 0;  // destination already exists / I/O error
  std::vector<std::string> errors;
};

// Move entries (groups expand to their members). Creates target directories.
// Never overwrites an existing file. Appends one line per moved file to
// <appDir>/data/fix.log: "<ts> | <old_rel> -> <new_rel>" (manual undo trail).
FixResult applyFixes(const std::vector<FixEntry> &entries,
                     const std::string &romsDir, const std::string &appDir);

// Canonical summary for byte-for-byte comparison with
// tools/fixer_replica.py:
//   COUNTS MOVE=<n> WARN=<n> GROUPS=<n> FILES=<n>
//   M|<system>|<relPath>|<expected>|<size>
//   G|<system>|<relDir>|<stem>|<expected>|<members>|<size>
//   W|<system>|<relPath>|<ext>|<note>
std::string fixSummary(const std::vector<FixEntry> &entries);

}  // namespace GameSync
