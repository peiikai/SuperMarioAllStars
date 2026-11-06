// S-DSP sample-level emulation. Models BRR decoding, gaussian interpolation,
// ADSR/GAIN envelopes, noise, pitch modulation and the echo FIR unit.
#include "dsp.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

enum { kRelease = 0, kAttack = 1, kDecay = 2, kSustain = 3 };

// Voice register offsets
enum { vVolL = 0, vVolR, vPitchL, vPitchH, vSrcn, vAdsr1, vAdsr2, vGain, vEnvx, vOutx };
// Global registers
enum {
  rMvolL = 0x0c, rMvolR = 0x1c, rEvolL = 0x2c, rEvolR = 0x3c, rKon = 0x4c, rKoff = 0x5c,
  rFlg = 0x6c, rEndx = 0x7c, rEfb = 0x0d, rPmon = 0x2d, rNon = 0x3d, rEon = 0x4d,
  rDir = 0x5d, rEsa = 0x6d, rEdl = 0x7d, rFir = 0x0f
};

constexpr int kCounterRange = 2048 * 5 * 3;

const uint16_t kCounterRates[32] = {
    kCounterRange + 1, 2048, 1536, 1280, 1024, 768, 640, 512, 384, 320, 256,
    192, 160, 128, 96, 80, 64, 48, 40, 32, 24, 20, 16, 12, 10, 8, 6, 5, 4, 3, 2, 1};
const uint16_t kCounterOffsets[32] = {
    1, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536,
    0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 536, 0, 1040, 0, 0};

// Interpolation kernel. Generated from a gaussian window fitted to the
// hardware response (4 taps, unity gain ~2048).
struct GaussTable {
  int16_t t[512];
  GaussTable() {
    const double k = 1.255;  // exp(-k*d^2)
    for (int f = 0; f < 256; f++) {
      double tt = f / 256.0;
      double w0 = std::exp(-k * (1 + tt) * (1 + tt));
      double w1 = std::exp(-k * tt * tt);
      double w2 = std::exp(-k * (1 - tt) * (1 - tt));
      double w3 = std::exp(-k * (2 - tt) * (2 - tt));
      double sum = w0 + w1 + w2 + w3;
      double s = 2040.0 / sum;
      // index mapping: [255-f]=w0 (dist 1+t), [511-f]=w1 (t), [256+f]=w2 (1-t), [f]=w3 (2-t)
      t[255 - f] = (int16_t)std::lround(w0 * s);
      t[511 - f] = (int16_t)std::lround(w1 * s);
      t[256 + f] = (int16_t)std::lround(w2 * s);
      t[f] = (int16_t)std::lround(w3 * s);
    }
  }
};
const GaussTable kGauss;

inline int clamp16(int v) {
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return v;
}

}  // namespace

void Dsp::reset() {
  std::memset(regs, 0, sizeof(regs));
  regs[rFlg] = 0xe0;
  for (auto& v : voices) v = Voice();
  counter_ = 0;
  everyOther_ = false;
  newKon_ = kon_ = koff_ = 0;
  noise_ = 0x4000;
  echoOffset_ = echoLength_ = 0;
  std::memset(echoHist_, 0, sizeof(echoHist_));
  echoHistPos_ = 0;
}

void Dsp::write(uint8_t adr, uint8_t val) {
  if (adr >= 0x80) return;
  regs[adr] = val;
  switch (adr & 0x0f) {
    case vEnvx: break;
    case vOutx: break;
    default: break;
  }
  if (adr == rKon) newKon_ = val;
  if (adr == rEndx) regs[rEndx] = 0;
}

bool Dsp::readCounter(int rate) const {
  // true when the event should fire this sample
  return ((unsigned)counter_ + kCounterOffsets[rate]) % kCounterRates[rate] == 0;
}

void Dsp::runEnvelope(Voice& v) {
  const uint8_t* vr = &regs[(&v - voices) * 0x10];
  int env = v.env;
  if (v.envMode == kRelease) {
    env -= 0x8;
    if (env < 0) env = 0;
    v.env = env;
    return;
  }
  int rate;
  int envData = vr[vAdsr2];
  uint8_t adsr0 = vr[vAdsr1];
  if (adsr0 & 0x80) {
    if (v.envMode >= kDecay) {
      env--;
      env -= env >> 8;
      rate = envData & 0x1f;
      if (v.envMode == kDecay) rate = ((adsr0 >> 3) & 0x0e) + 0x10;
    } else {
      rate = (adsr0 & 0x0f) * 2 + 1;
      env += rate < 31 ? 0x20 : 0x400;
    }
  } else {
    envData = vr[vGain];
    int mode = envData >> 5;
    if (mode < 4) {
      env = envData * 0x10;
      rate = 31;
    } else {
      rate = envData & 0x1f;
      if (mode == 4) {
        env -= 0x20;
      } else if (mode < 6) {
        env--;
        env -= env >> 8;
      } else {
        env += 0x20;
        if (mode > 6 && (unsigned)v.hiddenEnv >= 0x600) env += 0x8 - 0x20;
      }
    }
  }
  if ((env >> 8) == (envData >> 5) && v.envMode == kDecay) v.envMode = kSustain;
  v.hiddenEnv = env;
  if ((unsigned)env > 0x7ff) {
    env = env < 0 ? 0 : 0x7ff;
    if (v.envMode == kAttack) v.envMode = kDecay;
  }
  if (readCounter(rate)) v.env = env;
}

void Dsp::decodeBrr(Voice& v) {
  int header = ram_[v.brrAddr];
  int b0 = ram_[(v.brrAddr + v.brrOffset) & 0xffff];
  int b1 = ram_[(v.brrAddr + v.brrOffset + 1) & 0xffff];
  int nybbles = (b0 << 8) | b1;
  int shift = header >> 4;
  int filter = header & 0x0c;
  int* pos = &v.buf[v.bufPos];
  for (int i = 0; i < 4; i++) {
    int s = (int16_t)(nybbles & 0xf000) >> 12;
    nybbles <<= 4;
    s = (s << shift) >> 1;
    if (shift >= 0xd) s = (s >> 25) << 11;
    // previous samples (buffer holds 12 entries, duplicated at +12)
    int idx = v.bufPos + i;
    int p1 = v.buf[(idx + 11) % 12];
    int p2 = v.buf[(idx + 10) % 12] >> 1;
    if (filter >= 8) {
      s += p1;
      s -= p2;
      if (filter == 8) {
        s += p2 >> 4;
        s += (p1 * -3) >> 6;
      } else {
        s += (p1 * -13) >> 7;
        s += (p2 * 3) >> 4;
      }
    } else if (filter) {
      s += p1 >> 1;
      s += (-p1) >> 5;
    }
    s = clamp16(s);
    s = (int16_t)(s * 2);
    v.buf[idx % 12] = s;
    v.buf[idx % 12 + 12] = s;
  }
  (void)pos;
  v.bufPos += 4;
  if (v.bufPos >= 12) v.bufPos = 0;
}

int Dsp::interpolate(const Voice& v) const {
  int offset = (v.interpPos >> 4) & 0xff;
  const int16_t* fwd = kGauss.t + 255 - offset;
  const int16_t* rev = kGauss.t + offset;
  const int* in = &v.buf[(v.interpPos >> 12) + v.bufPos];
  int out;
  out = (fwd[0] * in[0]) >> 11;
  out += (fwd[256] * in[1]) >> 11;
  out += (rev[256] * in[2]) >> 11;
  out = (int16_t)out;
  out += (rev[0] * in[3]) >> 11;
  out = clamp16(out);
  return out & ~1;
}

void Dsp::runSample(int16_t* outL, int16_t* outR) {
  everyOther_ = !everyOther_;
  if (everyOther_) {
    newKon_ &= ~kon_;
    kon_ = newKon_;
    koff_ = regs[rKoff];
  }
  if (--counter_ < 0) counter_ = kCounterRange - 1;

  if (readCounter(regs[rFlg] & 0x1f)) {
    int feedback = (noise_ << 13) ^ (noise_ << 14);
    noise_ = (feedback & 0x4000) ^ (noise_ >> 1);
  }

  int mainL = 0, mainR = 0, echoL = 0, echoR = 0;
  int pmonInput = 0;
  uint8_t dir = regs[rDir];
  for (int vi = 0; vi < 8; vi++) {
    Voice& v = voices[vi];
    uint8_t* vr = &regs[vi * 0x10];
    int vbit = 1 << vi;
    int brrHeader = ram_[v.brrAddr];
    bool kon = (kon_ & vbit) && everyOther_;

    int pitch = (vr[vPitchL] | (vr[vPitchH] << 8)) & 0x3fff;
    if ((regs[rPmon] & vbit) && vi > 0) pitch += ((pmonInput >> 5) * pitch) >> 10;

    if (v.konDelay) {
      if (v.konDelay == 5) {
        int dirAdr = (dir * 0x100 + vr[vSrcn] * 4) & 0xffff;
        v.brrAddr = ram_[dirAdr] | (ram_[(dirAdr + 1) & 0xffff] << 8);
        v.brrOffset = 1;
        v.bufPos = 0;
        brrHeader = 0;
      }
      v.env = 0;
      v.hiddenEnv = 0;
      v.interpPos = 0;
      if (--v.konDelay & 3) v.interpPos = 0x4000;
      pitch = 0;
    }

    int output = interpolate(v);
    if (regs[rNon] & vbit) output = (int16_t)(noise_ * 2);
    int tOut = ((output * v.env) >> 11) & ~1;
    if (muteMask & vbit) tOut = 0;

    if ((regs[rFlg] & 0x80) || (brrHeader & 3) == 1) {
      v.envMode = kRelease;
      v.env = 0;
    }
    if (everyOther_) {
      if (koff_ & vbit) v.envMode = kRelease;
      if (kon) {
        v.konDelay = 5;
        v.envMode = kAttack;
        regs[rEndx] &= ~vbit;
      }
    }
    if (!v.konDelay) runEnvelope(v);

    // output mixing
    int l = (tOut * (int8_t)vr[vVolL]) >> 7;
    int r = (tOut * (int8_t)vr[vVolR]) >> 7;
    if (musicGain != 256 || sfxGain != 256) {  // (the player's music / sound effects volumes)
      const int g = (sfxVoices & vbit) ? sfxGain : musicGain;
      l = (l * g) >> 8;
      r = (r * g) >> 8;
    }
    mainL = clamp16(mainL + l);
    mainR = clamp16(mainR + r);
    if (regs[rEon] & vbit) {
      echoL = clamp16(echoL + l);
      echoR = clamp16(echoR + r);
    }
    pmonInput = tOut;
    v.lastOut = tOut;
    vr[vOutx] = (uint8_t)(tOut >> 8);
    vr[vEnvx] = (uint8_t)(v.env >> 4);

    // BRR decode
    if (v.interpPos >= 0x4000) {
      decodeBrr(v);
      v.brrOffset += 2;
      if (v.brrOffset >= 9) {
        v.brrAddr = (v.brrAddr + 9) & 0xffff;
        if (brrHeader & 1) {
          int dirAdr = (dir * 0x100 + vr[vSrcn] * 4 + 2) & 0xffff;
          v.brrAddr = ram_[dirAdr] | (ram_[(dirAdr + 1) & 0xffff] << 8);
          regs[rEndx] |= vbit;
        }
        v.brrOffset = 1;
      }
    }
    v.interpPos = (v.interpPos & 0x3fff) + pitch;
    if (v.interpPos > 0x7fff) v.interpPos = 0x7fff;
  }

  // Echo
  int echoAdr = (regs[rEsa] * 0x100 + echoOffset_) & 0xffff;
  if (echoOffset_ == 0) echoLength_ = (regs[rEdl] & 0x0f) * 0x800;
  echoOffset_ += 4;
  if (echoOffset_ >= echoLength_) echoOffset_ = 0;

  int inL = (int16_t)(ram_[echoAdr] | (ram_[(echoAdr + 1) & 0xffff] << 8));
  int inR = (int16_t)(ram_[(echoAdr + 2) & 0xffff] | (ram_[(echoAdr + 3) & 0xffff] << 8));
  echoHistPos_ = (echoHistPos_ + 1) & 7;
  echoHist_[echoHistPos_][0] = echoHist_[echoHistPos_ + 8][0] = inL >> 1;
  echoHist_[echoHistPos_][1] = echoHist_[echoHistPos_ + 8][1] = inR >> 1;

  int firL = 0, firR = 0;
  for (int i = 0; i < 7; i++) {
    int8_t c = (int8_t)regs[rFir + i * 0x10];
    firL += (echoHist_[echoHistPos_ + i + 1][0] * c) >> 6;
    firR += (echoHist_[echoHistPos_ + i + 1][1] * c) >> 6;
  }
  firL = (int16_t)firL;
  firR = (int16_t)firR;
  {
    int8_t c = (int8_t)regs[rFir + 7 * 0x10];
    firL += (int16_t)((echoHist_[echoHistPos_ + 8][0] * c) >> 6);
    firR += (int16_t)((echoHist_[echoHistPos_ + 8][1] * c) >> 6);
  }
  firL = clamp16(firL) & ~1;
  firR = clamp16(firR) & ~1;

  int outl = (int16_t)((mainL * (int8_t)regs[rMvolL]) >> 7);
  int outr = (int16_t)((mainR * (int8_t)regs[rMvolR]) >> 7);
  outl += (int16_t)((firL * (int8_t)regs[rEvolL]) >> 7);
  outr += (int16_t)((firR * (int8_t)regs[rEvolR]) >> 7);
  outl = clamp16(outl);
  outr = clamp16(outr);

  if (!(regs[rFlg] & 0x20)) {
    int el = clamp16(echoL + (int16_t)((firL * (int8_t)regs[rEfb]) >> 7)) & ~1;
    int er = clamp16(echoR + (int16_t)((firR * (int8_t)regs[rEfb]) >> 7)) & ~1;
    ram_[echoAdr] = el & 0xff;
    ram_[(echoAdr + 1) & 0xffff] = (el >> 8) & 0xff;
    ram_[(echoAdr + 2) & 0xffff] = er & 0xff;
    ram_[(echoAdr + 3) & 0xffff] = (er >> 8) & 0xff;
  }

  if (regs[rFlg] & 0x40) outl = outr = 0;
  *outL = (int16_t)outl;
  *outR = (int16_t)outr;
}

// ---------------------------------------------------------------------------
// State serialization: a flat copy of everything except the RAM pointer.

struct DspFlat {
  uint8_t regs[128];
  Dsp::Voice voices[8];
  int counter, everyOther;
  uint8_t newKon, kon, koff;
  int noise, echoOffset, echoLength;
  int echoHist[16][2];
  int echoHistPos;
};

int Dsp::stateSize() { return (int)sizeof(DspFlat); }

void Dsp::saveState(uint8_t* dst) const {
  DspFlat f;
  std::memcpy(f.regs, regs, 128);
  for (int i = 0; i < 8; i++) f.voices[i] = voices[i];
  f.counter = counter_;
  f.everyOther = everyOther_;
  f.newKon = newKon_;
  f.kon = kon_;
  f.koff = koff_;
  f.noise = noise_;
  f.echoOffset = echoOffset_;
  f.echoLength = echoLength_;
  std::memcpy(f.echoHist, echoHist_, sizeof(echoHist_));
  f.echoHistPos = echoHistPos_;
  std::memcpy(dst, &f, sizeof(f));
}

void Dsp::loadState(const uint8_t* src) {
  DspFlat f;
  std::memcpy(&f, src, sizeof(f));
  std::memcpy(regs, f.regs, 128);
  for (int i = 0; i < 8; i++) voices[i] = f.voices[i];
  counter_ = f.counter;
  everyOther_ = f.everyOther != 0;
  newKon_ = f.newKon;
  kon_ = f.kon;
  koff_ = f.koff;
  noise_ = f.noise;
  echoOffset_ = f.echoOffset;
  echoLength_ = f.echoLength;
  std::memcpy(echoHist_, f.echoHist, sizeof(echoHist_));
  echoHistPos_ = f.echoHistPos;
  // Bring file data into range: these index the sample buffers and ARAM.
  auto s16 = [](int& x) { x = std::clamp(x, -32768, 32767); };
  for (Voice& v : voices) {
    for (int& b : v.buf) s16(b);
    v.bufPos = (std::clamp(v.bufPos, 0, 11) / 4) * 4;
    v.interpPos = std::clamp(v.interpPos, 0, 0x7fff);
    v.brrAddr &= 0xffff;
    v.brrOffset = std::clamp(v.brrOffset, 1, 9);
    v.konDelay = std::clamp(v.konDelay, 0, 5);
    v.envMode &= 3;
    v.env = std::clamp(v.env, 0, 0x7ff);
    v.hiddenEnv = std::clamp(v.hiddenEnv, 0, 0x7ff);
    s16(v.lastOut);
  }
  counter_ = std::clamp(counter_, 0, kCounterRange - 1);
  noise_ &= 0x7fff;
  echoLength_ = std::clamp(echoLength_, 0, 0x7800) & ~3;
  echoOffset_ = std::clamp(echoOffset_, 0, 0x7800) & ~3;
  for (auto& h : echoHist_) {
    s16(h[0]);
    s16(h[1]);
  }
  echoHistPos_ &= 7;
}
