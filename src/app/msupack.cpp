// MSU-1 music packs and ROM patches: see msupack.h.
#include "app/msupack.h"
#include <algorithm>
#include <cwctype>
#include "app/sys.h"

namespace {
uint32_t crc32(const uint8_t* d, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t v = i;
      for (int k = 0; k < 8; k++) v = (v & 1) ? 0xedb88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ d[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

bool applyBps(const std::vector<uint8_t>& p, std::vector<uint8_t>& rom, std::string* err) {
  auto fail = [&](const char* why) {
    if (err) *err = why;
    return false;
  };
  if (p.size() < 4 + 3 + 12) return fail("the BPS patch is too short");
  const size_t end = p.size() - 12;
  if (crc32(p.data(), p.size() - 4) != get32(&p[p.size() - 4])) return fail("the BPS patch is damaged");
  if (crc32(rom.data(), rom.size()) != get32(&p[end])) return fail("the patch is for another version of the ROM");
  size_t at = 4;
  bool bad = false;
  auto num = [&]() -> uint64_t {
    uint64_t v = 0, shift = 1;
    while (true) {
      if (at >= end || shift > (1ull << 56)) {
        bad = true;
        return 0;
      }
      const uint8_t x = p[at++];
      v += (x & 0x7f) * shift;
      if (x & 0x80) break;
      shift <<= 7;
      v += shift;
    }
    return v;
  };
  const uint64_t srcSize = num(), dstSize = num(), metaSize = num();
  if (bad || srcSize != rom.size() || dstSize > (64u << 20) || metaSize > end - at) return fail("the BPS patch is damaged");
  at += (size_t)metaSize;
  std::vector<uint8_t> out((size_t)dstSize);
  size_t o = 0;
  int64_t srcRel = 0, dstRel = 0;
  while (at < end) {
    const uint64_t d = num();
    if (bad) return fail("the BPS patch is damaged");
    const uint64_t len = (d >> 2) + 1;
    if (len > out.size() - o) return fail("the BPS patch is damaged");
    switch (d & 3) {
      case 0:  // (from the source, same place)
        if (o + len > rom.size()) return fail("the BPS patch is damaged");
        std::copy_n(rom.begin() + o, (size_t)len, out.begin() + o);
        o += (size_t)len;
        break;
      case 1:  // (from the patch)
        if (len > end - at) return fail("the BPS patch is damaged");
        std::copy_n(p.begin() + at, (size_t)len, out.begin() + o);
        at += (size_t)len, o += (size_t)len;
        break;
      case 2:
      case 3: {  // (from the source / what's written so far, elsewhere)
        const uint64_t off = num();
        if (bad) return fail("the BPS patch is damaged");
        int64_t& rel = (d & 3) == 2 ? srcRel : dstRel;
        rel += (off & 1 ? -1 : 1) * (int64_t)(off >> 1);
        // (a target copy may overlap what it writes: each byte only needs to exist when read)
        const bool fromSrc = (d & 3) == 2;
        if (rel < 0 || (fromSrc ? (uint64_t)rel + len > rom.size() : (uint64_t)rel >= o))
          return fail("the BPS patch is damaged");
        for (uint64_t i = 0; i < len; i++) out[o++] = (d & 3) == 2 ? rom[(size_t)rel++] : out[(size_t)rel++];
        break;
      }
    }
  }
  if (o != out.size() || crc32(out.data(), out.size()) != get32(&p[end + 4])) return fail("the BPS patch is damaged");
  rom.swap(out);
  return true;
}

bool applyIps(const std::vector<uint8_t>& p, std::vector<uint8_t>& rom, std::string* err) {
  auto fail = [&]() {
    if (err) *err = "the IPS patch is damaged";
    return false;
  };
  size_t at = 5;
  std::vector<uint8_t> out = rom;
  while (true) {
    if (at + 3 > p.size()) return fail();
    if (p[at] == 'E' && p[at + 1] == 'O' && p[at + 2] == 'F') {
      at += 3;
      if (at + 3 <= p.size()) out.resize((size_t)(p[at] << 16 | p[at + 1] << 8 | p[at + 2]));  // (a truncation)
      break;
    }
    if (at + 5 > p.size()) return fail();
    const size_t off = (size_t)(p[at] << 16 | p[at + 1] << 8 | p[at + 2]);
    size_t len = (size_t)(p[at + 3] << 8 | p[at + 4]);
    at += 5;
    const uint8_t* src = nullptr;
    uint8_t fill = 0;
    if (len == 0) {  // (one byte, repeated)
      if (at + 3 > p.size()) return fail();
      len = (size_t)(p[at] << 8 | p[at + 1]);
      fill = p[at + 2];
      at += 3;
    } else {
      if (at + len > p.size()) return fail();
      src = &p[at];
      at += len;
    }
    if (off + len > out.size()) out.resize(off + len, 0);
    for (size_t i = 0; i < len; i++) out[off + i] = src ? src[i] : fill;
  }
  rom.swap(out);
  return true;
}

std::wstring lower(std::wstring s) {
  for (auto& c : s) c = (wchar_t)std::towlower(c);
  return s;
}

bool endsWith(const std::wstring& s, const wchar_t* tail) {
  const std::wstring t = tail;
  return s.size() >= t.size() && lower(s.substr(s.size() - t.size())) == t;
}

std::wstring findPatch(const std::wstring& dir) {
  std::vector<std::wstring> names;
  if (!listFiles(dir, names)) return L"";
  std::sort(names.begin(), names.end());
  for (const auto& n : names)
    if (endsWith(n, L".bps") || endsWith(n, L".ips")) return pathJoin(dir, n);
  return L"";
}
}  // namespace

bool applyRomPatch(const std::vector<uint8_t>& patch, std::vector<uint8_t>& rom, std::string* err) {
  if (patch.size() >= 4 && std::equal(patch.begin(), patch.begin() + 4, "BPS1")) return applyBps(patch, rom, err);
  if (patch.size() >= 5 && std::equal(patch.begin(), patch.begin() + 5, "PATCH")) return applyIps(patch, rom, err);
  if (err) *err = "not a BPS or IPS patch";
  return false;
}

std::vector<std::wstring> listMsuPacks(const std::wstring& msuDir) {
  std::vector<std::wstring> names;
  listDirs(msuDir, names);
  std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return lower(a) < lower(b); });
  return names;
}

bool findMsuPack(const std::wstring& msuDir, const std::wstring& name, MsuPack& out, std::string* err) {
  out = MsuPack();
  const std::wstring dir = pathJoin(msuDir, name);
  std::vector<std::wstring> names;
  if (!listFiles(dir, names)) {
    if (err) *err = "its folder is missing";
    return false;
  }
  std::sort(names.begin(), names.end());
  // (the tracks: NAME-N.pcm; NAME from the .msu file if there is one, else the most common)
  std::wstring base;
  for (const auto& n : names)
    if (endsWith(n, L".msu")) base = n.substr(0, n.size() - 4);
  std::vector<std::pair<std::wstring, int>> counts;
  for (const auto& n : names) {
    if (!endsWith(n, L".pcm")) continue;
    const size_t dash = n.find_last_of(L'-');
    if (dash == std::wstring::npos || dash + 1 >= n.size() - 4) continue;
    bool digits = true;
    for (size_t i = dash + 1; i < n.size() - 4; i++) digits = digits && std::iswdigit(n[i]);
    if (!digits) continue;
    const std::wstring b = n.substr(0, dash);
    auto it = std::find_if(counts.begin(), counts.end(), [&](const auto& c) { return lower(c.first) == lower(b); });
    if (it == counts.end()) counts.push_back({b, 1});
    else it->second++;
  }
  int tracks = 0;
  for (const auto& c : counts) {
    const bool pick = base.empty() ? c.second > tracks : lower(c.first) == lower(base);
    if (!pick) continue;
    tracks = c.second;
    out.base = pathJoin(dir, c.first);
  }
  if (!tracks) {
    if (err) *err = "no tracks (NAME-1.pcm, NAME-2.pcm, ...) in its folder";
    return false;
  }
  out.tracks = tracks;
  out.patch = findPatch(dir);
  if (out.patch.empty()) out.patch = findPatch(msuDir);
  if (out.patch.empty()) {
    if (err) *err = "no MSU-1 patch (.bps or .ips) in its folder";
    return false;
  }
  return true;
}
