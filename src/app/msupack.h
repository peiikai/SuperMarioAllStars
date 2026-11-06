// MSU-1 music packs: a folder in the msu folder holding an MSU-1 patch (.bps or
// .ips, for the USA ROM without a copier header) and its tracks (NAME-N.pcm,
// and NAME.msu if it has one). The patch is applied to the ROM as it's loaded
// (the file on disk is never changed) and core/msu1.h plays the tracks.
// A patch can also sit in the msu folder itself, for packs that came without one.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A BPS or IPS patch applied to `rom` (no copier header). False, with why, if
// it isn't one or (BPS) it's for another ROM.
bool applyRomPatch(const std::vector<uint8_t>& patch, std::vector<uint8_t>& rom, std::string* err);

// The pack folders' names (sorted).
std::vector<std::wstring> listMsuPacks(const std::wstring& msuDir);

struct MsuPack {
  std::wstring patch;  // the patch file
  std::wstring base;   // its tracks' path without "-N.pcm"
  int tracks = 0;
};
// The pack's patch and tracks; false, with why, if something's missing.
bool findMsuPack(const std::wstring& msuDir, const std::wstring& name, MsuPack& out, std::string* err);
