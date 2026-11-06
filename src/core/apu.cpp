// SPC700 interpreter, timers and the boot-ROM upload protocol (HLE).
#include "apu.h"
#include "statefix.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
const uint8_t kCycles[256] = {
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 6, 8,   // 0x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 4, 6,   // 1x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 4, 5, 4,   // 2x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 6, 5, 2, 2, 3, 8,   // 3x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 6, 6,   // 4x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 4, 5, 2, 2, 4, 3,   // 5x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 4, 5, 5,   // 6x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 6,   // 7x
    2, 8, 4, 5, 3, 4, 3, 6, 2, 6, 5, 4, 5, 2, 4, 5,   // 8x
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 12, 5,  // 9x
    3, 8, 4, 5, 3, 4, 3, 6, 2, 6, 4, 4, 5, 2, 4, 4,   // Ax
    2, 8, 4, 5, 4, 5, 5, 6, 5, 5, 5, 5, 2, 2, 3, 4,   // Bx
    3, 8, 4, 5, 4, 5, 4, 7, 2, 5, 6, 4, 5, 2, 4, 9,   // Cx
    2, 8, 4, 5, 5, 6, 6, 7, 4, 5, 5, 5, 2, 2, 6, 3,   // Dx
    2, 8, 4, 5, 3, 4, 3, 6, 2, 4, 5, 3, 4, 3, 4, 3,   // Ex
    2, 8, 4, 5, 4, 5, 5, 6, 3, 4, 5, 4, 2, 2, 4, 3,   // Fx
};
}  // namespace

Apu::Apu() : dsp(ram) { reset(true); }

void Apu::reset(bool hard) {
  if (hard) {
    // Power-on RAM pattern is not important; use a simple fill.
    for (int i = 0; i < 0x10000; i++) ram[i] = (i & 0x20) ? 0xff : 0x00;
  }
  r = Regs();
  r.pc = 0xffc0;
  control_ = 0x80;
  dspAddr_ = 0;
  aux_[0] = aux_[1] = 0;
  for (auto& t : timers_) t = Timer();
  std::memset(inPorts, 0, 4);
  std::memset(outPorts, 0, 4);
  dsp.reset();
  iplState_ = 0;
  iplY_ = 0;
  sampleCounter_ = 0;
  cycles_ = 0;
  samples.clear();
}

uint8_t Apu::getPsw() const {
  return (r.n ? 0x80 : 0) | (r.v ? 0x40 : 0) | (r.p ? 0x20 : 0) | (r.b ? 0x10 : 0) |
         (r.h ? 0x08 : 0) | (r.i ? 0x04 : 0) | (r.z ? 0x02 : 0) | (r.c ? 0x01 : 0);
}

void Apu::setPsw(uint8_t v) {
  r.n = v & 0x80;
  r.v = v & 0x40;
  r.p = v & 0x20;
  r.b = v & 0x10;
  r.h = v & 0x08;
  r.i = v & 0x04;
  r.z = v & 0x02;
  r.c = v & 0x01;
}

uint8_t Apu::aluAdc(uint8_t a, uint8_t b) {
  int res = a + b + (r.c ? 1 : 0);
  r.v = (~(a ^ b) & (a ^ res) & 0x80) != 0;
  r.h = ((a ^ b ^ res) & 0x10) != 0;
  r.c = res > 0xff;
  setNZ((uint8_t)res);
  return (uint8_t)res;
}

uint8_t Apu::read(uint16_t adr) {
  if ((adr & 0xfff0) == 0x00f0) {
    switch (adr) {
      case 0xf0: return 0;
      case 0xf1: return 0;
      case 0xf2: return dspAddr_;
      case 0xf3: return dsp.read(dspAddr_);
      case 0xf4: case 0xf5: case 0xf6: case 0xf7: return inPorts[adr - 0xf4];
      case 0xf8: return aux_[0];
      case 0xf9: return aux_[1];
      case 0xfa: case 0xfb: case 0xfc: return 0;
      case 0xfd: case 0xfe: case 0xff: {
        Timer& t = timers_[adr - 0xfd];
        uint8_t v = t.out & 0x0f;
        t.out = 0;
        return v;
      }
    }
  }
  if (adr >= 0xffc0 && (control_ & 0x80)) {
    // Boot ROM area: only the reset vector is meaningful for software.
    if (adr == 0xfffe) return 0xc0;
    if (adr == 0xffff) return 0xff;
    return ram[adr];
  }
  return ram[adr];
}

void Apu::write(uint16_t adr, uint8_t v) {
  ram[adr] = v;
  if ((adr & 0xfff0) != 0x00f0) return;
  switch (adr) {
    case 0xf0: break;  // TEST - ignored
    case 0xf1: {
      for (int i = 0; i < 3; i++) {
        bool en = v & (1 << i);
        if (en && !timers_[i].enabled) {
          timers_[i].internal = 0;
          timers_[i].out = 0;
        }
        timers_[i].enabled = en;
      }
      if (v & 0x10) inPorts[0] = inPorts[1] = 0;
      if (v & 0x20) inPorts[2] = inPorts[3] = 0;
      control_ = v;
      break;
    }
    case 0xf2: dspAddr_ = v; break;
    case 0xf3: dsp.write(dspAddr_, v); break;
    case 0xf4: case 0xf5: case 0xf6: case 0xf7: outPorts[adr - 0xf4] = v; break;
    case 0xf8: aux_[0] = v; break;
    case 0xf9: aux_[1] = v; break;
    case 0xfa: case 0xfb: case 0xfc: timers_[adr - 0xfa].target = v; break;
    default: break;
  }
}

void Apu::tickTimers(int c) {
  for (int i = 0; i < 3; i++) {
    Timer& t = timers_[i];
    int period = i == 2 ? 16 : 128;
    t.divider += c;
    while (t.divider >= period) {
      t.divider -= period;
      if (!t.enabled) continue;
      t.internal++;
      if (t.internal == t.target) {  // target 0 == 256 (wraps to 0)
        t.internal = 0;
        t.out = (t.out + 1) & 0x0f;
      }
    }
  }
}

void Apu::runTo(uint64_t target) {
  while (cycles_ < target) {
    bool ipl = r.pc >= 0xffc0 && (control_ & 0x80);
    // A jump back into the boot ROM (e.g. a driver returning control for a new
    // upload) restarts the protocol from its "ready" announcement.
    if (ipl && !inIpl_ && r.pc == 0xffc0) iplState_ = 0;
    inIpl_ = ipl;
    int c = ipl ? iplStep() : step();
    cycles_ += c;
    tickTimers(c);
    sampleCounter_ += c;
    while (sampleCounter_ >= 32) {
      sampleCounter_ -= 32;
      int16_t l, rr;
      // (All-Stars' sound driver, N-SPC "Ver S1.20", the same in all five
      // titles, marks the voices a sound effect holds in its RAM at $1A: each
      // effect port's handler sets its voice's bit while the effect plays and
      // clears it after, and the music code leaves those voices alone. So
      // the rest are the music's. See docs/re/sound.md.)
      if (!ipl && (dsp.musicGain != 256 || dsp.sfxGain != 256)) dsp.sfxVoices = ram[0x1a];
      dsp.runSample(&l, &rr);
      if (generateAudio) {
        samples.push_back(l);
        samples.push_back(rr);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Boot ROM, implemented as a state machine that follows the documented
// upload protocol: announce $AA/$BB, wait for $CC, then receive blocks.

int Apu::iplStep() {
  auto getAdr = [&]() { return uint16_t(ram[0x00] | (ram[0x01] << 8)); };
  switch (iplState_) {
    case 0: {  // power-on: clear zero page, announce ready
      for (int i = 0xef; i >= 1; i--) ram[i] = 0;
      r.sp = 0xef;
      r.a = 0;
      r.x = 0;
      outPorts[0] = 0xaa;
      outPorts[1] = 0xbb;
      iplState_ = 1;
      return 1200;
    }
    case 1:  // wait for $CC
      if (inPorts[0] == 0xcc) iplState_ = 2;
      return 5;
    case 2: {  // start of a block / command
      ram[0x00] = inPorts[2];
      ram[0x01] = inPorts[3];
      uint8_t idx = inPorts[0];
      uint8_t cmd = inPorts[1];
      outPorts[0] = idx;
      if (cmd != 0) {
        iplState_ = 3;
        return 10;
      }
      // Execute: jump to address.
      r.a = 0;
      r.x = 0;
      r.y = 0;
      r.z = true;
      r.n = false;
      r.pc = getAdr();
      iplState_ = 2;
      return 10;
    }
    case 3:  // wait for first index (0)
      if (inPorts[0] == 0) {
        iplY_ = 0;
        iplState_ = 4;
      }
      return 5;
    case 4: {  // data loop: wait for the host to post index Y
      uint8_t p0 = inPorts[0];
      if (p0 == iplY_) {
        // The data byte is read a few cycles after the index matched,
        // giving the host time to finish writing port 1.
        iplState_ = 5;
        return 5;
      }
      if (((uint8_t)(iplY_ - p0)) & 0x80) {
        // host skipped ahead: new command
        iplState_ = 2;
      }
      return 5;
    }
    case 5: {
      uint8_t data = inPorts[1];
      outPorts[0] = iplY_;
      ram[(getAdr() + iplY_) & 0xffff] = data;
      iplY_++;
      if (iplY_ == 0) ram[0x01]++;
      iplState_ = 4;
      return 7;
    }
  }
  return 2;
}

int Apu::branch(bool cond) {
  int8_t off = (int8_t)fetch();
  if (cond) {
    r.pc += off;
    return 2;
  }
  return 0;
}

// ---------------------------------------------------------------------------

int Apu::step() {
  if (r.stopped) return 2;
  if (traceFile && traceLimit > 0) {
    traceLimit--;
    fprintf((FILE*)traceFile, "%04X %02X %02X %02X  A:%02X X:%02X Y:%02X SP:%02X PSW:%02X\n", r.pc, ram[r.pc],
            ram[(r.pc + 1) & 0xffff], ram[(r.pc + 2) & 0xffff], r.a, r.x, r.y, r.sp, getPsw());
  }
  uint8_t op = fetch();
  int cyc = kCycles[op];
  switch (op) {
    // ---- NOP / flags
    case 0x00: break;
    case 0x20: r.p = false; break;
    case 0x40: r.p = true; break;
    case 0x60: r.c = false; break;
    case 0x80: r.c = true; break;
    case 0xa0: r.i = true; break;
    case 0xc0: r.i = false; break;
    case 0xe0: r.v = false; r.h = false; break;
    case 0xed: r.c = !r.c; break;
    case 0xef: case 0xff: r.stopped = true; r.pc--; break;

    // ---- TCALL n
    case 0x01: case 0x11: case 0x21: case 0x31: case 0x41: case 0x51: case 0x61: case 0x71:
    case 0x81: case 0x91: case 0xa1: case 0xb1: case 0xc1: case 0xd1: case 0xe1: case 0xf1: {
      int n = op >> 4;
      push(r.pc >> 8);
      push(r.pc & 0xff);
      uint16_t v = 0xffde - n * 2;
      r.pc = read(v) | (read(v + 1) << 8);
      break;
    }
    // ---- SET1 / CLR1 d.b
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xa2: case 0xc2: case 0xe2: {
      uint8_t a = fetch();
      writeDp(a, readDp(a) | (1 << (op >> 5)));
      break;
    }
    case 0x12: case 0x32: case 0x52: case 0x72: case 0x92: case 0xb2: case 0xd2: case 0xf2: {
      uint8_t a = fetch();
      writeDp(a, readDp(a) & ~(1 << (op >> 5)));
      break;
    }
    // ---- BBS / BBC
    case 0x03: case 0x23: case 0x43: case 0x63: case 0x83: case 0xa3: case 0xc3: case 0xe3: {
      uint8_t a = fetch();
      uint8_t v = readDp(a);
      cyc += branch(v & (1 << (op >> 5)));
      break;
    }
    case 0x13: case 0x33: case 0x53: case 0x73: case 0x93: case 0xb3: case 0xd3: case 0xf3: {
      uint8_t a = fetch();
      uint8_t v = readDp(a);
      cyc += branch(!(v & (1 << (op >> 5))));
      break;
    }

    // ---- ALU groups: OR(0x00) AND(0x20) EOR(0x40) CMP(0x60) ADC(0x80) SBC(0xa0)
#define ALU_OP(fn, base)                                                          \
  case base + 0x04: r.a = fn(r.a, readDp(fetch())); break;                        \
  case base + 0x05: r.a = fn(r.a, read(fetch16())); break;                        \
  case base + 0x06: r.a = fn(r.a, readDp(r.x)); break;                            \
  case base + 0x07: {                                                             \
    uint8_t d = fetch() + r.x;                                                    \
    uint16_t p = readDp(d) | (readDp(uint8_t(d + 1)) << 8);                       \
    r.a = fn(r.a, read(p));                                                       \
    break;                                                                        \
  }                                                                               \
  case base + 0x08: r.a = fn(r.a, fetch()); break;                                \
  case base + 0x09: {                                                             \
    uint8_t s = readDp(fetch());                                                  \
    uint8_t da = fetch();                                                         \
    uint8_t v = fn(readDp(da), s);                                                \
    if (base != 0x60) writeDp(da, v);                                             \
    break;                                                                        \
  }                                                                               \
  case base + 0x14: r.a = fn(r.a, readDp(uint8_t(fetch() + r.x))); break;         \
  case base + 0x15: r.a = fn(r.a, read(uint16_t(fetch16() + r.x))); break;        \
  case base + 0x16: r.a = fn(r.a, read(uint16_t(fetch16() + r.y))); break;        \
  case base + 0x17: {                                                             \
    uint8_t d = fetch();                                                          \
    uint16_t p = readDp(d) | (readDp(uint8_t(d + 1)) << 8);                       \
    r.a = fn(r.a, read(uint16_t(p + r.y)));                                       \
    break;                                                                        \
  }                                                                               \
  case base + 0x18: {                                                             \
    uint8_t imm = fetch();                                                        \
    uint8_t da = fetch();                                                         \
    uint8_t v = fn(readDp(da), imm);                                              \
    if (base != 0x60) writeDp(da, v);                                             \
    break;                                                                        \
  }                                                                               \
  case base + 0x19: {                                                             \
    uint8_t s = readDp(r.y);                                                      \
    uint8_t v = fn(readDp(r.x), s);                                               \
    if (base != 0x60) writeDp(r.x, v);                                            \
    break;                                                                        \
  }

    ALU_OP(aluOr, 0x00)
    ALU_OP(aluAnd, 0x20)
    ALU_OP(aluEor, 0x40)
    ALU_OP(aluCmp, 0x60)
    ALU_OP(aluAdc, 0x80)
    ALU_OP(aluSbc, 0xa0)
#undef ALU_OP

    // ---- bit ops on absolute m.b
    case 0x0a: {  // OR1 C,m.b
      uint16_t w = fetch16();
      bool bit = (read(w & 0x1fff) >> (w >> 13)) & 1;
      r.c = r.c || bit;
      break;
    }
    case 0x2a: {  // OR1 C,/m.b
      uint16_t w = fetch16();
      bool bit = (read(w & 0x1fff) >> (w >> 13)) & 1;
      r.c = r.c || !bit;
      break;
    }
    case 0x4a: {  // AND1 C,m.b
      uint16_t w = fetch16();
      bool bit = (read(w & 0x1fff) >> (w >> 13)) & 1;
      r.c = r.c && bit;
      break;
    }
    case 0x6a: {  // AND1 C,/m.b
      uint16_t w = fetch16();
      bool bit = (read(w & 0x1fff) >> (w >> 13)) & 1;
      r.c = r.c && !bit;
      break;
    }
    case 0x8a: {  // EOR1 C,m.b
      uint16_t w = fetch16();
      bool bit = (read(w & 0x1fff) >> (w >> 13)) & 1;
      r.c = r.c != bit;
      break;
    }
    case 0xaa: {  // MOV1 C,m.b
      uint16_t w = fetch16();
      r.c = (read(w & 0x1fff) >> (w >> 13)) & 1;
      break;
    }
    case 0xca: {  // MOV1 m.b,C
      uint16_t w = fetch16();
      uint16_t a = w & 0x1fff;
      uint8_t v = read(a);
      int b = w >> 13;
      v = r.c ? (v | (1 << b)) : (v & ~(1 << b));
      write(a, v);
      break;
    }
    case 0xea: {  // NOT1 m.b
      uint16_t w = fetch16();
      uint16_t a = w & 0x1fff;
      write(a, read(a) ^ (1 << (w >> 13)));
      break;
    }

    // ---- shifts
    case 0x0b: { uint8_t a = fetch(); writeDp(a, aluAsl(readDp(a))); break; }
    case 0x0c: { uint16_t a = fetch16(); write(a, aluAsl(read(a))); break; }
    case 0x1b: { uint8_t a = fetch() + r.x; writeDp(a, aluAsl(readDp(a))); break; }
    case 0x1c: r.a = aluAsl(r.a); break;
    case 0x2b: { uint8_t a = fetch(); writeDp(a, aluRol(readDp(a))); break; }
    case 0x2c: { uint16_t a = fetch16(); write(a, aluRol(read(a))); break; }
    case 0x3b: { uint8_t a = fetch() + r.x; writeDp(a, aluRol(readDp(a))); break; }
    case 0x3c: r.a = aluRol(r.a); break;
    case 0x4b: { uint8_t a = fetch(); writeDp(a, aluLsr(readDp(a))); break; }
    case 0x4c: { uint16_t a = fetch16(); write(a, aluLsr(read(a))); break; }
    case 0x5b: { uint8_t a = fetch() + r.x; writeDp(a, aluLsr(readDp(a))); break; }
    case 0x5c: r.a = aluLsr(r.a); break;
    case 0x6b: { uint8_t a = fetch(); writeDp(a, aluRor(readDp(a))); break; }
    case 0x6c: { uint16_t a = fetch16(); write(a, aluRor(read(a))); break; }
    case 0x7b: { uint8_t a = fetch() + r.x; writeDp(a, aluRor(readDp(a))); break; }
    case 0x7c: r.a = aluRor(r.a); break;

    // ---- inc / dec
    case 0x8b: { uint8_t a = fetch(); writeDp(a, aluDec(readDp(a))); break; }
    case 0x8c: { uint16_t a = fetch16(); write(a, aluDec(read(a))); break; }
    case 0x9b: { uint8_t a = fetch() + r.x; writeDp(a, aluDec(readDp(a))); break; }
    case 0x9c: r.a = aluDec(r.a); break;
    case 0xab: { uint8_t a = fetch(); writeDp(a, aluInc(readDp(a))); break; }
    case 0xac: { uint16_t a = fetch16(); write(a, aluInc(read(a))); break; }
    case 0xbb: { uint8_t a = fetch() + r.x; writeDp(a, aluInc(readDp(a))); break; }
    case 0xbc: r.a = aluInc(r.a); break;
    case 0x1d: r.x = aluDec(r.x); break;
    case 0x3d: r.x = aluInc(r.x); break;
    case 0xdc: r.y = aluDec(r.y); break;
    case 0xfc: r.y = aluInc(r.y); break;

    // ---- 16-bit
    case 0x1a: {  // DECW
      uint8_t a = fetch();
      uint16_t v = readDp(a) | (readDp(uint8_t(a + 1)) << 8);
      v--;
      writeDp(a, v & 0xff);
      writeDp(uint8_t(a + 1), v >> 8);
      r.n = v & 0x8000;
      r.z = v == 0;
      break;
    }
    case 0x3a: {  // INCW
      uint8_t a = fetch();
      uint16_t v = readDp(a) | (readDp(uint8_t(a + 1)) << 8);
      v++;
      writeDp(a, v & 0xff);
      writeDp(uint8_t(a + 1), v >> 8);
      r.n = v & 0x8000;
      r.z = v == 0;
      break;
    }
    case 0x5a: {  // CMPW YA,d
      uint8_t a = fetch();
      uint16_t m = readDp(a) | (readDp(uint8_t(a + 1)) << 8);
      uint16_t ya = (r.y << 8) | r.a;
      int res = ya - m;
      r.c = res >= 0;
      r.n = res & 0x8000;
      r.z = (res & 0xffff) == 0;
      break;
    }
    case 0x7a: case 0x9a: {  // ADDW / SUBW
      uint8_t a = fetch();
      int m = readDp(a) | (readDp(uint8_t(a + 1)) << 8);
      if (op == 0x9a) m = (m ^ 0xffff) + 1;
      int ya = (r.y << 8) | r.a;
      int sum = ya + m;
      r.c = sum > 0xffff;
      r.v = (~(ya ^ m) & (ya ^ sum) & 0x8000) != 0;
      r.h = ((ya ^ m ^ sum) & 0x1000) != 0;
      sum &= 0xffff;
      r.n = sum & 0x8000;
      r.z = sum == 0;
      r.a = sum & 0xff;
      r.y = sum >> 8;
      break;
    }
    case 0xba: {  // MOVW YA,d
      uint8_t a = fetch();
      r.a = readDp(a);
      r.y = readDp(uint8_t(a + 1));
      r.n = r.y & 0x80;
      r.z = r.a == 0 && r.y == 0;
      break;
    }
    case 0xda: {  // MOVW d,YA
      uint8_t a = fetch();
      writeDp(a, r.a);
      writeDp(uint8_t(a + 1), r.y);
      break;
    }
    case 0xcf: {  // MUL YA
      uint16_t v = r.y * r.a;
      r.a = v & 0xff;
      r.y = v >> 8;
      setNZ(r.y);
      break;
    }
    case 0x9e: {  // DIV YA,X
      unsigned ya = (r.y << 8) | r.a;
      r.v = r.y >= r.x;
      r.h = (r.y & 15) >= (r.x & 15);
      unsigned a, y;
      if (r.y < (unsigned)r.x * 2) {
        a = r.x ? ya / r.x : 0x1ff;
        y = r.x ? ya - a * r.x : r.y;
      } else {
        a = 255 - (ya - r.x * 0x200) / (256 - r.x);
        y = r.x + (ya - r.x * 0x200) % (256 - r.x);
      }
      r.a = (uint8_t)a;
      r.y = (uint8_t)y;
      setNZ(r.a);
      break;
    }
    case 0x9f: r.a = (r.a >> 4) | (r.a << 4); setNZ(r.a); break;  // XCN
    case 0xdf: {  // DAA
      if (r.c || r.a > 0x99) { r.a += 0x60; r.c = true; }
      if (r.h || (r.a & 0x0f) > 9) r.a += 6;
      setNZ(r.a);
      break;
    }
    case 0xbe: {  // DAS
      if (!r.c || r.a > 0x99) { r.a -= 0x60; r.c = false; }
      if (!r.h || (r.a & 0x0f) > 9) r.a -= 6;
      setNZ(r.a);
      break;
    }

    // ---- compares with X/Y
    case 0x1e: aluCmp(r.x, read(fetch16())); break;
    case 0x3e: aluCmp(r.x, readDp(fetch())); break;
    case 0xc8: aluCmp(r.x, fetch()); break;
    case 0x5e: aluCmp(r.y, read(fetch16())); break;
    case 0x7e: aluCmp(r.y, readDp(fetch())); break;
    case 0xad: aluCmp(r.y, fetch()); break;

    // ---- branches / jumps
    case 0x10: cyc += branch(!r.n); break;
    case 0x30: cyc += branch(r.n); break;
    case 0x50: cyc += branch(!r.v); break;
    case 0x70: cyc += branch(r.v); break;
    case 0x90: cyc += branch(!r.c); break;
    case 0xb0: cyc += branch(r.c); break;
    case 0xd0: cyc += branch(!r.z); break;
    case 0xf0: cyc += branch(r.z); break;
    case 0x2f: branch(true); break;
    case 0x2e: {  // CBNE d,r
      uint8_t v = readDp(fetch());
      cyc += branch(r.a != v);
      break;
    }
    case 0xde: {  // CBNE d+X,r
      uint8_t v = readDp(uint8_t(fetch() + r.x));
      cyc += branch(r.a != v);
      break;
    }
    case 0x6e: {  // DBNZ d,r
      uint8_t a = fetch();
      uint8_t v = readDp(a) - 1;
      writeDp(a, v);
      cyc += branch(v != 0);
      break;
    }
    case 0xfe: r.y--; cyc += branch(r.y != 0); break;
    case 0x5f: r.pc = fetch16(); break;
    case 0x1f: {  // JMP [!a+X]
      uint16_t a = fetch16() + r.x;
      r.pc = read(a) | (read(uint16_t(a + 1)) << 8);
      break;
    }
    case 0x3f: {  // CALL
      uint16_t a = fetch16();
      push(r.pc >> 8);
      push(r.pc & 0xff);
      r.pc = a;
      break;
    }
    case 0x4f: {  // PCALL
      uint8_t a = fetch();
      push(r.pc >> 8);
      push(r.pc & 0xff);
      r.pc = 0xff00 | a;
      break;
    }
    case 0x6f: {  // RET
      uint8_t l = pop();
      r.pc = l | (pop() << 8);
      break;
    }
    case 0x7f: {  // RETI
      setPsw(pop());
      uint8_t l = pop();
      r.pc = l | (pop() << 8);
      break;
    }
    case 0x0f: {  // BRK
      push(r.pc >> 8);
      push(r.pc & 0xff);
      push(getPsw());
      r.b = true;
      r.i = false;
      r.pc = read(0xffde) | (read(0xffdf) << 8);
      break;
    }

    // ---- stack
    case 0x0d: push(getPsw()); break;
    case 0x2d: push(r.a); break;
    case 0x4d: push(r.x); break;
    case 0x6d: push(r.y); break;
    case 0x8e: setPsw(pop()); break;
    case 0xae: r.a = pop(); break;
    case 0xce: r.x = pop(); break;
    case 0xee: r.y = pop(); break;

    // ---- test-and-set
    case 0x0e: case 0x4e: {
      uint16_t a = fetch16();
      uint8_t v = read(a);
      uint8_t t = r.a - v;
      r.n = t & 0x80;
      r.z = t == 0;
      write(a, op == 0x0e ? (v | r.a) : (v & ~r.a));
      break;
    }

    // ---- moves
    case 0x5d: r.x = r.a; setNZ(r.x); break;
    case 0x7d: r.a = r.x; setNZ(r.a); break;
    case 0x9d: r.x = r.sp; setNZ(r.x); break;
    case 0xbd: r.sp = r.x; break;
    case 0xdd: r.a = r.y; setNZ(r.a); break;
    case 0xfd: r.y = r.a; setNZ(r.y); break;
    case 0x8d: r.y = fetch(); setNZ(r.y); break;
    case 0xcd: r.x = fetch(); setNZ(r.x); break;
    case 0xe8: r.a = fetch(); setNZ(r.a); break;
    case 0x8f: {  // MOV d,#i
      uint8_t imm = fetch();
      uint8_t a = fetch();
      writeDp(a, imm);
      break;
    }
    case 0xfa: {  // MOV dd,ds
      uint8_t s = readDp(fetch());
      writeDp(fetch(), s);
      break;
    }
    case 0xaf: writeDp(r.x, r.a); r.x++; break;           // MOV (X)+,A
    case 0xbf: r.a = readDp(r.x); r.x++; setNZ(r.a); break;  // MOV A,(X)+
    case 0xc4: writeDp(fetch(), r.a); break;
    case 0xc5: write(fetch16(), r.a); break;
    case 0xc6: writeDp(r.x, r.a); break;
    case 0xc7: {
      uint8_t d = fetch() + r.x;
      uint16_t p = readDp(d) | (readDp(uint8_t(d + 1)) << 8);
      write(p, r.a);
      break;
    }
    case 0xc9: write(fetch16(), r.x); break;
    case 0xcb: writeDp(fetch(), r.y); break;
    case 0xcc: write(fetch16(), r.y); break;
    case 0xd4: writeDp(uint8_t(fetch() + r.x), r.a); break;
    case 0xd5: write(uint16_t(fetch16() + r.x), r.a); break;
    case 0xd6: write(uint16_t(fetch16() + r.y), r.a); break;
    case 0xd7: {
      uint8_t d = fetch();
      uint16_t p = readDp(d) | (readDp(uint8_t(d + 1)) << 8);
      write(uint16_t(p + r.y), r.a);
      break;
    }
    case 0xd8: writeDp(fetch(), r.x); break;
    case 0xd9: writeDp(uint8_t(fetch() + r.y), r.x); break;
    case 0xdb: writeDp(uint8_t(fetch() + r.x), r.y); break;
    case 0xe4: r.a = readDp(fetch()); setNZ(r.a); break;
    case 0xe5: r.a = read(fetch16()); setNZ(r.a); break;
    case 0xe6: r.a = readDp(r.x); setNZ(r.a); break;
    case 0xe7: {
      uint8_t d = fetch() + r.x;
      uint16_t p = readDp(d) | (readDp(uint8_t(d + 1)) << 8);
      r.a = read(p);
      setNZ(r.a);
      break;
    }
    case 0xe9: r.x = read(fetch16()); setNZ(r.x); break;
    case 0xeb: r.y = readDp(fetch()); setNZ(r.y); break;
    case 0xec: r.y = read(fetch16()); setNZ(r.y); break;
    case 0xf4: r.a = readDp(uint8_t(fetch() + r.x)); setNZ(r.a); break;
    case 0xf5: r.a = read(uint16_t(fetch16() + r.x)); setNZ(r.a); break;
    case 0xf6: r.a = read(uint16_t(fetch16() + r.y)); setNZ(r.a); break;
    case 0xf7: {
      uint8_t d = fetch();
      uint16_t p = readDp(d) | (readDp(uint8_t(d + 1)) << 8);
      r.a = read(uint16_t(p + r.y));
      setNZ(r.a);
      break;
    }
    case 0xf8: r.x = readDp(fetch()); setNZ(r.x); break;
    case 0xf9: r.x = readDp(uint8_t(fetch() + r.y)); setNZ(r.x); break;
    case 0xfb: r.y = readDp(uint8_t(fetch() + r.x)); setNZ(r.y); break;

    default: break;
  }
  return cyc;
}

// ---------------------------------------------------------------------------

namespace {
struct ApuFlatHdr {
  uint64_t cycles;
  int sampleCounter;
  uint8_t control, dspAddr, aux0, aux1;
  uint8_t inPorts[4], outPorts[4];
  int iplState;
  uint8_t iplY;
};
}  // namespace

std::vector<uint8_t> Apu::saveState() const {
  std::vector<uint8_t> out;
  auto put = [&](const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    out.insert(out.end(), b, b + n);
  };
  ApuFlatHdr h{};
  h.cycles = cycles_;
  h.sampleCounter = sampleCounter_;
  h.control = control_;
  h.dspAddr = dspAddr_;
  h.aux0 = aux_[0];
  h.aux1 = aux_[1];
  std::memcpy(h.inPorts, inPorts, 4);
  std::memcpy(h.outPorts, outPorts, 4);
  h.iplState = iplState_;
  h.iplY = iplY_;
  put(&h, sizeof(h));
  put(&r, sizeof(r));
  put(timers_, sizeof(timers_));
  put(ram, sizeof(ram));
  std::vector<uint8_t> d(Dsp::stateSize());
  dsp.saveState(d.data());
  put(d.data(), d.size());
  return out;
}

bool Apu::loadState(const uint8_t* data, size_t size) {
  size_t need = sizeof(ApuFlatHdr) + sizeof(r) + sizeof(timers_) + sizeof(ram) + Dsp::stateSize();
  if (size < need) return false;
  const uint8_t* p = data;
  ApuFlatHdr h;
  std::memcpy(&h, p, sizeof(h));
  p += sizeof(h);
  cycles_ = h.cycles;
  sampleCounter_ = h.sampleCounter;
  control_ = h.control;
  dspAddr_ = h.dspAddr;
  aux_[0] = h.aux0;
  aux_[1] = h.aux1;
  std::memcpy(inPorts, h.inPorts, 4);
  std::memcpy(outPorts, h.outPorts, 4);
  iplState_ = h.iplState;
  iplY_ = h.iplY;
  std::memcpy(&r, p, sizeof(r));
  p += sizeof(r);
  std::memcpy(timers_, p, sizeof(timers_));
  p += sizeof(timers_);
  std::memcpy(ram, p, sizeof(ram));
  p += sizeof(ram);
  dsp.loadState(p);
  // Bring file data into range (see statefix.h).
  for (bool* b : {&r.n, &r.v, &r.p, &r.b, &r.h, &r.i, &r.z, &r.c, &r.stopped}) fixBool(*b);
  for (Timer& t : timers_) {
    fixBool(t.enabled);
    t.divider = std::clamp(t.divider, 0, 255);
  }
  iplState_ = std::clamp(iplState_, 0, 5);
  sampleCounter_ = std::clamp(sampleCounter_, 0, 31);
  // Not stored in states: whether the boot ROM (IPL) was already running.
  // Derive it, or a state saved mid-upload would restart the IPL handshake.
  inIpl_ = r.pc >= 0xffc0 && (control_ & 0x80);
  return true;
}
