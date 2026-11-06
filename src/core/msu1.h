// MSU-1: the SD2SNES / higan "media enhancement chip" that MSU-1 patches use
// for CD-quality music (and a data file). Registers at $2000-$2007 (banks
// $00-$3F, $80-$BF):
//   read  $2000 status: 7 data busy, 6 audio busy, 5 repeating, 4 playing,
//               3 track missing, 2-0 revision
//         $2001 the data file's next byte
//         $2002-$2007 "S-MSU1"
//   write $2000-$2003 the data file's seek offset ($2003 seeks)
//         $2004-$2005 the track ($2005 loads it)
//         $2006 volume, $2007 control (0 play, 1 repeat)
// Tracks are <base>-<n>.pcm: "MSU1", a 32-bit loop point (in samples), then
// 44.1 kHz 16-bit stereo; the data file <base>.msu. Mixed into the console's
// own sound (32040 Hz) as each frame's samples are taken.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

class Msu1 {
public:
  ~Msu1();
  // `base`: the pack's files' path without their endings (folder/sma_msu1).
  void open(const std::wstring& base);
  void reset();
  uint8_t read(uint16_t adr);
  void write(uint16_t adr, uint8_t v);
  // The music for `frames` stereo samples at the console's rate, added in.
  void mix(int16_t* samples, size_t frames, double rate);
  bool playing() const { return playing_; }
  void setGain(float g) { gain_ = g; }  // (the player's volume for it, on top of the game's)
  int track() const { return trackNum_; }
  // (save states: the track, where it is, the data file's place)
  std::vector<uint8_t> saveState() const;
  void loadState(const uint8_t* p, size_t n);

private:
  std::wstring base_;
  FILE* data_ = nullptr;
  uint32_t dataPos_ = 0, seekLatch_ = 0;
  FILE* track_ = nullptr;
  uint16_t trackLatch_ = 0, trackNum_ = 0;
  bool missing_ = false, playing_ = false, repeat_ = false;
  uint8_t volume_ = 255;
  float gain_ = 1.0f;
  uint32_t loop_ = 0;          // (the loop point, in samples)
  uint64_t trackFrames_ = 0;   // (its length, in samples)
  uint64_t pos_ = 0;           // (the next sample to read)
  // (44.1 kHz to the console's rate: the two samples either side, and how far between)
  int16_t prev_[2] = {}, cur_[2] = {};
  double frac_ = 1.0;
  std::vector<int16_t> buf_;
  size_t bufAt_ = 0;
  void loadTrack(uint16_t n, uint64_t at, bool keepPlaying);
  bool nextSample(int16_t* lr);
};
