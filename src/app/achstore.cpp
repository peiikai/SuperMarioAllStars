#include "app/achstore.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include "app/sys.h"
#include "core/fileio.h"

void AchievementStore::load(const std::wstring& path) {
  path_ = path;
  unlocked_.clear();
  progress_.clear();
  noticeShown = false;
  std::string text;
  readText(path, text);
  std::istringstream f(text);
  std::string line, section;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == ';') continue;
    if (line[0] == '[') {
      section = line.substr(1, line.find(']') - 1);
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (section == "unlocked") unlocked_[k] = v;
    else if (section == "progress") progress_[k] = atoi(v.c_str());
    else if (section == "notice" && k == "shown") noticeShown = v == "1";
  }
}

bool AchievementStore::save() const {
  std::ostringstream f;
  f << "; Super Mario All-Stars PC port: achievements earned\n";
  f << "[notice]\nshown=" << (noticeShown ? 1 : 0) << "\n";
  f << "[unlocked]\n";
  for (auto& kv : unlocked_) f << kv.first << "=" << kv.second << "\n";
  f << "[progress]\n";
  for (auto& kv : progress_) f << kv.first << "=" << kv.second << "\n";
  std::string t = f.str();
  return writeFileSafely(path_, t.data(), t.size());
}

std::string AchievementStore::unlockedOn(const std::string& id) const { return unlockedAt(id).substr(0, 10); }

std::string AchievementStore::unlockedAt(const std::string& id) const {
  auto it = unlocked_.find(id);
  return it == unlocked_.end() ? std::string() : it->second;
}

bool AchievementStore::unlock(const std::string& id) {
  if (unlocked_.count(id)) return false;
  unlocked_[id] = timeText("%Y-%m-%d %H:%M:%S");
  return true;
}

int AchievementStore::progress(const std::string& id) const {
  auto it = progress_.find(id);
  return it == progress_.end() ? 0 : it->second;
}

bool AchievementStore::setProgress(const std::string& id, int value) {
  int& v = progress_[id];
  if (value <= v) return false;
  v = value;
  return true;
}
