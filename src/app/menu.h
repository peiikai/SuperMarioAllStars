// Overlay menu system drawn on a Canvas (canvas.h): stacked pages of
// items (actions, toggles, choices, sliders, key bindings, section headers),
// toast messages and the "achievement unlocked" banner.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "app/canvas.h"

struct MenuInput {
  bool up = false, down = false, left = false, right = false;
  bool confirm = false, back = false;
  bool tabLeft = false, tabRight = false;
  bool anyPressed() const { return up || down || left || right || confirm || back || tabLeft || tabRight; }
};

struct MenuItem {
  // Separator: a section header (its label, small, in the accent colour), or
  // a blank line when it has none. Label: a read-only row (value on the right).
  enum Kind { Action, Choice, Slider, Toggle, Submenu, Bind, Label, Separator };
  Kind kind = Action;
  std::string label;
  std::string help;
  std::function<void()> onActivate;               // Action / Submenu / Bind
  std::function<int()> get;                       // Choice/Slider/Toggle value
  std::function<void(int)> set;
  std::function<std::vector<std::string>()> options;  // Choice labels
  std::function<std::string()> valueText;         // custom value display (a note beside a toggle)
  std::function<bool()> enabled;
  int minV = 0, maxV = 100, step = 1;
  std::string suffix;
  bool wrap = true;

  static MenuItem action(std::string l, std::function<void()> f, std::string h = "");
  static MenuItem submenu(std::string l, std::function<void()> f, std::string h = "");
  static MenuItem toggle(std::string l, std::function<int()> g, std::function<void(int)> s, std::string h = "");
  static MenuItem choice(std::string l, std::vector<std::string> opts, std::function<int()> g,
                         std::function<void(int)> s, std::string h = "");
  static MenuItem slider(std::string l, int mn, int mx, int st, std::function<int()> g, std::function<void(int)> s,
                         std::string suffix = "", std::string h = "");
  static MenuItem info(std::string l, std::string h = "");
  static MenuItem separator(std::string l = "");
  static MenuItem header(std::string l) { return separator(std::move(l)); }
};

class Menu;
struct MenuPage {
  std::string title;
  std::string subtitle;
  std::vector<MenuItem> items;
  int sel = 0;
  float scroll = 0;  // first row shown (eases towards the selection)
  bool freeScroll = false;  // (touch: dragged, so it doesn't follow the selection)
  bool wide = false;  // a wider panel (long lists, side panels)
  std::function<void(MenuPage&)> rebuild;  // called on enter / refresh
  // Optional custom content (drawn in place of the item list)
  std::function<void(Menu&, Canvas*, RectF)> drawCustom;
  std::function<bool(Menu&, const MenuInput&)> inputCustom;  // return true if consumed
  std::function<void()> onLeave;
  float listFraction = 1.0f;  // portion of the panel used by the list (rest: custom panel)
};

class Menu {
public:
  // The canvas to draw on (the video layer's; a new one after its device was rebuilt).
  void setCanvas(Canvas* c) { cv_ = c; }

  bool open() const { return !stack_.empty(); }
  void push(std::shared_ptr<MenuPage> p);
  void pop();
  void closeAll();
  MenuPage* top() { return stack_.empty() ? nullptr : stack_.back().get(); }
  size_t depth() const { return stack_.size(); }
  void refresh();
  // A Yes / No page (No selected) over the current one.
  void confirm(const std::string& question, std::function<void()> yes, const std::string& detail = "");

  // Rebinding capture state (set by the app)
  bool capturing = false;

  // Touch screens: a close button on the panel, and taps and drags on its rows.
  bool touchUi = false;
  std::wstring keysHint;  // the footer's key line (empty: the keyboard and pad's)
  bool confirmRight = false;  // (the pad's right face button confirms: the footer says so)
  void tap(float x, float y);
  void drag(float dy);
  std::string captureText;

  void update(const MenuInput& in, float dt);
  void draw(int w, int h, float anim);

  // The colour of titles, the selection and switches (the running game's).
  void setAccent(ColorF c) { accent_ = c; }
  // The menus' look: 0 Storybook (bright and chunky), 1 Retro (black, bricks,
  // a pixel font), 2 Themed Dark (the classic panel with the game's heading),
  // 3 Classic. And the game the heading shows (Themed Dark): 0 the
  // collection, 1 Super Mario Bros., 2 The Lost Levels, 3 SMB2, 4 SMB3.
  enum Style { kStorybook, kRetro, kThemedDark, kClassic, kStyleCount };
  int style = kStorybook;
  void setGameArt(int g) { gameArt_ = g; }

  // OSD notifications (visible whether or not the menu is open)
  void notify(const std::string& text, float seconds = 2.2f, bool hint = false);
  void tickToasts(float dt);  // call every frame, menu open or not
  // "Achievement unlocked": a gold card that slides down at the top; several queue up.
  void achievement(const std::string& name, const std::string& desc);
  bool bannerShowing() const { return !banners_.empty(); }
  // `status`: what the emulation is doing besides running (rewinding...), top left.
  void drawOsd(int w, int h, const std::string& fps, const std::string& status = std::string());

  // Drawing helpers for custom pages
  void text(const std::wstring& s, RectF r, float size, bool bold, ColorF c,
            TextAlign align = kAlignLeading,
            ParaAlign palign = kParaCenter);
  void text(const std::wstring& s, RectF r, float size, int weight, ColorF c,
            TextAlign align = kAlignLeading,
            ParaAlign palign = kParaCenter);
  float measure(const std::wstring& s, float size, bool bold);  // width in pixels
  float measure(const std::wstring& s, float size, int weight);
  void fillRound(RectF r, float radius, ColorF c);
  void outlineRound(RectF r, float radius, float width, ColorF c);
  Canvas* canvas() const { return cv_; }
  float uiScale() const { return scale_; }
  // (Retro: text in the 5x7 pixel font, upper case)
  void pixelText(const std::wstring& s, RectF r, float size, ColorF c, TextAlign align, ParaAlign palign);
  float pixelMeasure(const std::wstring& s, float size);
  // A title or heading in the style's display face (Luckiest Guy, outlined; or pixels).
  void displayText(const std::wstring& s, RectF r, float size, ColorF fill, ColorF edge, float edgeW,
                   TextAlign align = kAlignLeading);
  float displayMeasure(const std::wstring& s, float size);
  void star(float cx, float cy, float r, ColorF fill, ColorF edge, float edgeW) { drawStar(cx, cy, r, fill, edge, edgeW); }
  ColorF accent() const { return accent_; }
  static std::wstring widen(const std::string& s);

private:
  Canvas* cv_ = nullptr;
  int gameArt_ = 0;
  std::vector<std::shared_ptr<MenuPage>> stack_;
  float scale_ = 1.0f;
  float time_ = 0;  // for the selection's pulse
  ColorF accent_ = {0.91f, 0.72f, 0.29f, 1.0f};
  // key repeat
  float repeatTimer_ = 0;
  int repeatDir_ = 0;
  struct Toast {
    std::string text;
    float time;
    float total;
    bool hint = false;  // removed as soon as the menu opens
  };
  std::vector<Toast> toasts_;
  struct Banner {
    std::string name, desc;
    float t = 0;
  };
  std::vector<Banner> banners_;
  void roundRect(RectF r, float radius, ColorF c);
  void drawToasts(int w, int h, float s);
  void drawBanner(int w, int h, float s);
  void drawStar(float cx, float cy, float r, ColorF fill, ColorF edge, float edgeW);
  void drawHeaderArt(RectF r, float a);  // (Themed Dark: the game's pattern behind the title)
  void drawBricks(RectF r, ColorF brick, ColorF mortar, float bh, float a);
  void activate(MenuItem& it);
  void adjust(MenuItem& it, int dir);
  bool selectable(const MenuItem& it) const;
  void moveSel(int dir);
  // (where the last draw put each row, the close button)
  struct RowHit {
    RectF r;
    int index;
  };
  std::vector<RowHit> rowHits_;
  RectF closeHit_{};
  float rowH_ = 1;
};
