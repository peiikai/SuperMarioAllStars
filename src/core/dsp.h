// S-DSP (SNES sound generator) - sample-level model.
#pragma once
#include <cstdint>

class Dsp {
public:
  explicit Dsp(uint8_t* ram) : ram_(ram) {}
  void reset();
  void write(uint8_t adr, uint8_t val);
  uint8_t read(uint8_t adr) const { return regs[adr & 0x7f]; }
  // Generate one stereo sample.
  void runSample(int16_t* outL, int16_t* outR);

  uint8_t regs[128] = {};

  // Per-voice state (public for the music visualizer)
  struct Voice {
    int buf[24];      // decoded BRR samples (duplicated for wrap)
    int bufPos = 0;
    int interpPos = 0;
    int brrAddr = 0;
    int brrOffset = 1;
    int konDelay = 0;
    int envMode = 0;  // 0 release, 1 attack, 2 decay, 3 sustain
    int env = 0;
    int hiddenEnv = 0;
    int lastOut = 0;
  } voices[8];

  int voiceLevel(int v) const { return voices[v].env; }
  // Bitmask of voices muted by the user (for sound test / debugging)
  uint8_t muteMask = 0;
  // Music and sound effects at their own volumes (256 = as made): the voices
  // the sound driver has given to sound effects (see Apu::musicSfxGain) are at
  // sfxGain, the rest (the music) at musicGain. Only what is heard changes.
  uint8_t sfxVoices = 0;
  int musicGain = 256, sfxGain = 256;

  // Serialization helpers
  struct State;
  void saveState(uint8_t* dst) const;
  void loadState(const uint8_t* src);
  static int stateSize();

private:
  uint8_t* ram_;
  int counter_ = 0;
  bool everyOther_ = false;
  uint8_t newKon_ = 0, kon_ = 0, koff_ = 0;
  int noise_ = 0x4000;
  int echoOffset_ = 0, echoLength_ = 0;
  int echoHist_[16][2] = {};
  int echoHistPos_ = 0;

  bool readCounter(int rate) const;
  void runEnvelope(Voice& v);
  void decodeBrr(Voice& v);
  int interpolate(const Voice& v) const;
};
