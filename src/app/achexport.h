// Achievements exported for other tools: achievements.csv in the columns
// PlayniteAchievements' spreadsheet import reads (ID, Title, Description,
// Points, Trophy Type, Hidden, Rarity, Category, Progress, Progress Total,
// Unlocked, Unlock Time, Unlocked Icon, Locked Icon) and an icons folder with
// each achievement's badge, earned and locked (PNG, drawn here: the port's own
// pictures, a colour per game and a symbol per achievement).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class AchievementStore;
struct AchievementDef;

// Writes dir/achievements.csv and dir/icons/*.png; false, with why, if it couldn't.
bool exportAchievements(const std::wstring& dir, const AchievementStore& store, std::string* err);

// An achievement's badge, size x size pixels (0x00RRGGBB).
void drawAchievementIcon(const AchievementDef& d, bool locked, int size, std::vector<uint32_t>& px);

// Its tier (0 bronze, 1 silver, 2 gold, 3 platinum) and points.
int achievementTier(const char* id);
int achievementPoints(const char* id);
