#include "AppConfig.h"

#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

namespace GameSync {

namespace {

bool mkdirRecursive(const std::string &path) {
  if (path.empty()) return false;
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur += path[i];
    if (path[i] == '/' && cur.size() > 1) {
      mkdir(cur.c_str(), 0755);
    }
  }
  mkdir(path.c_str(), 0755);
  struct stat st{};
  return stat(path.c_str(), &st) == 0;
}

} // namespace

AppConfig &AppConfig::instance() {
  static AppConfig config;
  return config;
}

void AppConfig::setDataDirectory(const std::string &path) {
  m_dataDirectory = path;
  mkdirRecursive(m_dataDirectory);
}

std::string AppConfig::readValue(const std::string &name) const {
  std::ifstream stream(m_dataDirectory + "/" + name);
  std::string value;
  std::getline(stream, value);
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
    value.pop_back();
  }
  return value;
}

void AppConfig::writeValue(const std::string &name,
                           const std::string &value) const {
  mkdirRecursive(m_dataDirectory);
  std::ofstream stream(m_dataDirectory + "/" + name,
                       std::ios::trunc | std::ios::binary);
  stream << value << '\n';
}

std::string AppConfig::getSetting(const std::string &name,
                                  const std::string &fallback) const {
  const std::string value = readValue(name);
  return value.empty() ? fallback : value;
}

void AppConfig::setSetting(const std::string &name,
                           const std::string &value) const {
  writeValue(name, value);
}

} // namespace GameSync
