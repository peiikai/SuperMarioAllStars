// What has been earned: achievements.ini next to the program (the date of
// each one earned, progress counts, and whether the one-time explanation of
// achievements was shown).
#pragma once
#include <map>
#include <string>

class AchievementStore {
public:
  void load(const std::wstring& path);
  bool save() const;

  bool unlocked(const std::string& id) const { return unlocked_.count(id) != 0; }
  std::string unlockedOn(const std::string& id) const;  // "2026-10-17", or "" if not earned
  std::string unlockedAt(const std::string& id) const;  // "2026-10-17 14:05:09" (older: the day only), or ""
  bool unlock(const std::string& id);                   // true if newly earned
  int progress(const std::string& id) const;
  bool setProgress(const std::string& id, int value);   // keeps the highest; true if it grew
  int count() const { return (int)unlocked_.size(); }

  bool noticeShown = false;

private:
  std::wstring path_;
  std::map<std::string, std::string> unlocked_;  // id -> when earned (local time; the day only before 1.1)
  std::map<std::string, int> progress_;
};
