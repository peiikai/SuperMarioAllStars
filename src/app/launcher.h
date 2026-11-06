// Launcher: one card per game with a live attract-mode preview, plus Options /
// Achievements / Music Player / Credits / Quit (as the Mickey & Donald
// Collection's). Each preview is its own console running the All-Stars ROM,
// driven through the game select into that game's title (see
// SmasGame::requestTitle), silent unless its card is chosen.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "app/menu.h"
#include "game/smas.h"

class Audio;
class Snes;

class Launcher {
public:
  // The four games' cards (0-3), and Super Mario All-Stars itself (kAllStars:
  // its own title and game select, shown alone when chosen in Options).
  static constexpr int kCards = 4, kAllStars = 4, kButtons = 5;
  enum Action { kNone, kPlay, kOptions, kAchievements, kMusic, kCredits, kQuit, kPatreon, kUpdate };

  ~Launcher();
  // The ROM the previews run (null: not found, the cards say what is needed).
  // `sram`: the battery save, copied into each preview (never written back).
  void setRom(const std::vector<uint8_t>* rom, const std::string& fileName, bool exact, const uint8_t* sram,
              size_t sramSize);
  // Shapes and pictures belong to the canvas: dropped (and made again on a new one).
  void releaseGpu();
  void setVersion(const std::string& v) { version_ = v; }
  void setPixelAspect(double par) { par_ = par; }

  Action update(const MenuInput& in, float dt);
  int card() const { return cards_[lastCard_]; }  // (0-3, or kAllStars)
  static GameId cardGame(int i);                  // (kAllStars: GameId::Menu)
  void setAllStarsOnly(bool on);
  void selectGame(GameId g);
  // Emulates `frames` frames of each preview (and, while they start, as much
  // as a few milliseconds allow); the chosen card's sound when `sound`.
  // `refreshHz`: the display's; the cards not chosen get a share of each
  // refresh's time (0: no limit), the launcher kept smooth on slow devices.
  void runPreviews(int frames, bool sound, Audio& audio, double refreshHz = 0);
  void draw(Menu& menu, int w, int h);
  // Touch (and the mouse): a card tapped is chosen, tapped again played; a
  // button tapped is used; the logo or the Patreon page under it opens it.
  Action tap(float x, float y);
  bool overLink(float x, float y) const;  // (the logo and its page: a link)
  void setHint(const std::string& h) { hint_ = h; }  // the line along the bottom (empty: the keys')
  void setConfirmRight(bool on) { confirmRight_ = on; }
  void setUpdate(const std::string& tag) { updateTag_ = tag; }  // (a newer release: its badge, top right)
  void setGains(int music, int sfx) { musicGain_ = music, sfxGain_ = sfx; }  // (the previews' music / sound effects, 256 = as made)  // (the keys' line: the right face button plays)
  ColorF accent() const;
  static ColorF gameAccent(GameId g);
  void stopSound() { soundCard_ = -1; }
  // The player's own pictures (Options > Launcher; none are shipped): covers
  // for the cards, in place of their live previews (a card without one keeps
  // its preview), and a background, from `dir` (artName(card) or
  // "background", .png / .jpg / .jpeg). Read again when anything changed, or
  // `reload` (a picture chosen).
  void setArt(const std::wstring& dir, bool covers, bool background, bool reload = false);
  void setCoverFill(bool fill) { coverFill_ = fill; }  // covers fill their (square) frames, trimmed; else whole
  static const char* artName(int card);  // (smb1, lostlevels, smb2, smb3, allstars)
  bool hasCover(int card) const { return useCovers_ && card >= 0 && card <= kCards && cover_[card].w > 0; }

private:
  struct Preview {
    std::unique_ptr<Snes> snes;
    std::unique_ptr<SmasGame> game;
    bool ready = false;
    int bootFrames = 0;
    float shown = 0;  // fade-in
    int image = 0, imageH = 0;  // (made on the canvas of imageEpoch_)
    bool fresh = false;         // a new picture drawn since the image was last updated
    int owed = 0;               // (a card not chosen: frames still to run, see runPreviews)
  };
  int drawTurn_ = 0;  // (whose turn it is of the cards not chosen to draw a picture: see runPreviews)
  Preview prev_[kCards + 1];
  bool haveRom_ = false, exact_ = true;
  std::string romName_, version_ = "1.1.1";
  double par_ = 8.0 / 7.0;
  std::vector<int> cards_ = {0, 1, 2, 3};  // the cards shown
  const std::vector<uint8_t>* rom_ = nullptr;
  std::vector<uint8_t> sram_;
  int sel_ = 0, lastCard_ = 0;  // (slots: cards 0..N-1, then the buttons)
  float anim_[kCards + 1 + kButtons] = {};
  int repeatDir_ = 0;
  float repeatTimer_ = 0;
  float time_ = 0;  // (the chosen button's star bobs)
  int soundCard_ = -1;
  std::string hint_;
  bool confirmRight_ = false;
  int musicGain_ = 256, sfxGain_ = 256;
  std::vector<RectF> cardHits_, buttonHits_;  // (where the last draw put them)
  RectF linkHit_{}, updateHit_{};
  std::string updateTag_;
  std::vector<int16_t> buf_;
  void startPreview(int i);
  void stepPreview(Preview& p);
  // the background (see drawBackground), made for the window's size
  Canvas* canvas_ = nullptr;  // (the shapes' and pictures' canvas, and its epoch)
  unsigned epoch_ = 0;
  int floor_ = 0, star_ = 0;
  float bgW_ = 0, bgH_ = 0;
  void useCanvas(Canvas* cv);
  int logo_ = 0;  // (the Team RedTail logo's picture)
  struct Art {
    std::vector<uint32_t> px;  // (read from the file; made a picture on the canvas when first drawn)
    int w = 0, h = 0;
    int image = 0;
  };
  Art cover_[kCards + 1], bgArt_;
  std::wstring artDir_;
  bool useCovers_ = false, useBg_ = false, coverFill_ = false;
  void readArt(Art& a, const char* name);
  int artImage(Canvas* cv, Art& a);
  void freeArt();
  void drawBackground(Menu& menu, float W, float H);
  void releaseBackground();
};
