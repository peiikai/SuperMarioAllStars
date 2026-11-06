// MSU-1: see msu1.h.
#include "core/msu1.h"
#include <algorithm>
#include <cstring>

namespace {
constexpr size_t kChunk = 4096;  // (samples read at a time)

// (the core's own: the app's openFile isn't linked into smastool)
FILE* openFile(const std::wstring& path, const char* mode) {
#ifdef _WIN32
  std::wstring m(mode, mode + std::strlen(mode));
  return _wfopen(path.c_str(), m.c_str());
#else
  std::string u;
  for (wchar_t wc : path) {
    uint32_t c = (uint32_t)wc;
    if (c < 0x80) u += (char)c;
    else if (c < 0x800) u += (char)(0xc0 | c >> 6), u += (char)(0x80 | (c & 0x3f));
    else if (c < 0x10000) u += (char)(0xe0 | c >> 12), u += (char)(0x80 | (c >> 6 & 0x3f)), u += (char)(0x80 | (c & 0x3f));
    else
      u += (char)(0xf0 | c >> 18), u += (char)(0x80 | (c >> 12 & 0x3f)), u += (char)(0x80 | (c >> 6 & 0x3f)),
          u += (char)(0x80 | (c & 0x3f));
  }
  return fopen(u.c_str(), mode);
#endif
}

void put32(std::vector<uint8_t>& o, uint64_t v, int bytes) {
  for (int i = 0; i < bytes; i++) o.push_back((uint8_t)(v >> (8 * i)));
}

uint64_t get(const uint8_t*& p, int bytes) {
  uint64_t v = 0;
  for (int i = 0; i < bytes; i++) v |= (uint64_t)p[i] << (8 * i);
  p += bytes;
  return v;
}
}  // namespace

Msu1::~Msu1() {
  if (data_) fclose(data_);
  if (track_) fclose(track_);
}

void Msu1::open(const std::wstring& base) {
  base_ = base;
  if (data_) fclose(data_);
  data_ = openFile(base + L".msu", "rb");
  reset();
}

void Msu1::reset() {
  if (track_) fclose(track_);
  track_ = nullptr;
  dataPos_ = seekLatch_ = 0;
  trackLatch_ = trackNum_ = 0;
  missing_ = playing_ = repeat_ = false;
  volume_ = 255;
  loop_ = 0;
  trackFrames_ = pos_ = 0;
  prev_[0] = prev_[1] = cur_[0] = cur_[1] = 0;
  frac_ = 1.0;
  buf_.clear();
  bufAt_ = 0;
}

uint8_t Msu1::read(uint16_t adr) {
  switch (adr & 7) {
    case 0:
      return (uint8_t)((repeat_ ? 0x20 : 0) | (playing_ ? 0x10 : 0) | (missing_ ? 0x08 : 0) | 2);
    case 1: {
      uint8_t v = 0;
      if (data_ && fseek(data_, (long)dataPos_, SEEK_SET) == 0) {
        int c = fgetc(data_);
        if (c != EOF) v = (uint8_t)c;
      }
      dataPos_++;
      return v;
    }
    default: return (uint8_t)"S-MSU1"[(adr & 7) - 2];
  }
}

void Msu1::write(uint16_t adr, uint8_t v) {
  switch (adr & 7) {
    case 0:
    case 1:
    case 2:
      seekLatch_ = (seekLatch_ & ~(0xffu << (8 * (adr & 7)))) | ((uint32_t)v << (8 * (adr & 7)));
      break;
    case 3:
      seekLatch_ = (seekLatch_ & 0x00ffffff) | ((uint32_t)v << 24);
      dataPos_ = seekLatch_;
      break;
    case 4: trackLatch_ = (uint16_t)((trackLatch_ & 0xff00) | v); break;
    case 5:
      trackLatch_ = (uint16_t)((trackLatch_ & 0x00ff) | (v << 8));
      loadTrack(trackLatch_, 0, false);
      break;
    case 6: volume_ = v; break;
    case 7:
      if (missing_) break;
      playing_ = v & 1;
      repeat_ = (v & 2) != 0;
      break;
  }
}

// A track opened (missing: the status says so, the game plays its own music).
void Msu1::loadTrack(uint16_t n, uint64_t at, bool keepPlaying) {
  if (track_) fclose(track_);
  track_ = nullptr;
  trackNum_ = n;
  playing_ = keepPlaying;
  if (!keepPlaying) repeat_ = false;
  buf_.clear();
  bufAt_ = 0;
  frac_ = 1.0;
  prev_[0] = prev_[1] = cur_[0] = cur_[1] = 0;
  missing_ = true;
  loop_ = 0;
  trackFrames_ = pos_ = 0;
  if (base_.empty()) return;
  track_ = openFile(base_ + L"-" + std::to_wstring(n) + L".pcm", "rb");
  uint8_t head[8];
  if (!track_ || fread(head, 1, 8, track_) != 8 || std::memcmp(head, "MSU1", 4) != 0) {
    if (track_) fclose(track_);
    track_ = nullptr;
    playing_ = false;
    return;
  }
  loop_ = head[4] | head[5] << 8 | head[6] << 16 | (uint32_t)head[7] << 24;
  fseek(track_, 0, SEEK_END);
  long size = ftell(track_);
  trackFrames_ = size > 8 ? (uint64_t)(size - 8) / 4 : 0;
  missing_ = false;
  pos_ = std::min<uint64_t>(at, trackFrames_);
  fseek(track_, (long)(8 + pos_ * 4), SEEK_SET);
}

// The track's next sample (silence when stopped or done).
bool Msu1::nextSample(int16_t* lr) {
  if (!playing_ || !track_) {
    lr[0] = lr[1] = 0;
    return false;
  }
  if (bufAt_ >= buf_.size()) {
    if (pos_ >= trackFrames_) {
      if (!repeat_ || loop_ >= trackFrames_) {
        playing_ = false;
        lr[0] = lr[1] = 0;
        return false;
      }
      pos_ = loop_;
      fseek(track_, (long)(8 + pos_ * 4), SEEK_SET);
    }
    const size_t want = (size_t)std::min<uint64_t>(kChunk, trackFrames_ - pos_);
    buf_.resize(want * 2);
    const size_t got = fread(buf_.data(), 4, want, track_);
    buf_.resize(got * 2);
    bufAt_ = 0;
    if (got == 0) {
      playing_ = false;
      lr[0] = lr[1] = 0;
      return false;
    }
  }
  // (little-endian files, read as host shorts: x86 and ARM are little-endian)
  lr[0] = buf_[bufAt_];
  lr[1] = buf_[bufAt_ + 1];
  bufAt_ += 2;
  pos_++;
  return true;
}

void Msu1::mix(int16_t* s, size_t frames, double rate) {
  if (!playing_ && frac_ >= 1.0 && !cur_[0] && !cur_[1]) return;
  const double step = 44100.0 / rate;
  const double vol = volume_ * gain_ / 255.0;
  for (size_t i = 0; i < frames; i++) {
    while (frac_ >= 1.0) {
      prev_[0] = cur_[0];
      prev_[1] = cur_[1];
      nextSample(cur_);
      frac_ -= 1.0;
    }
    for (int c = 0; c < 2; c++) {
      const double v = prev_[c] + (cur_[c] - prev_[c]) * frac_;
      const int m = s[i * 2 + c] + (int)(v * vol);
      s[i * 2 + c] = (int16_t)std::clamp(m, -32768, 32767);
    }
    frac_ += step;
  }
}

std::vector<uint8_t> Msu1::saveState() const {
  std::vector<uint8_t> o;
  o.push_back(1);  // (version)
  put32(o, dataPos_, 4);
  put32(o, seekLatch_, 4);
  put32(o, trackLatch_, 2);
  put32(o, trackNum_, 2);
  o.push_back((uint8_t)((missing_ ? 1 : 0) | (playing_ ? 2 : 0) | (repeat_ ? 4 : 0)));
  o.push_back(volume_);
  put32(o, pos_, 8);
  return o;
}

void Msu1::loadState(const uint8_t* p, size_t n) {
  if (n < 23 || p[0] != 1) {
    // (a state without the chip's: silent until the game picks a track)
    loadTrack(0, 0, false);
    playing_ = false;
    return;
  }
  p++;
  dataPos_ = (uint32_t)get(p, 4);
  seekLatch_ = (uint32_t)get(p, 4);
  trackLatch_ = (uint16_t)get(p, 2);
  const uint16_t track = (uint16_t)get(p, 2);
  const uint8_t flags = *p++;
  volume_ = *p++;
  const uint64_t at = get(p, 8);
  const bool wasMissing = flags & 1;
  if (!wasMissing && track_ && !missing_ && track == trackNum_) {
    // (the same track, as rewinding steps back through it: a seek, not a reopen)
    playing_ = (flags & 2) != 0;
    repeat_ = (flags & 4) != 0;
    pos_ = std::min<uint64_t>(at, trackFrames_);
    fseek(track_, (long)(8 + pos_ * 4), SEEK_SET);
    buf_.clear();
    bufAt_ = 0;
    return;
  }
  if (wasMissing) {
    loadTrack(track, 0, false);
    missing_ = true;
    return;
  }
  repeat_ = (flags & 4) != 0;
  loadTrack(track, at, (flags & 2) != 0);
  repeat_ = (flags & 4) != 0;
}
