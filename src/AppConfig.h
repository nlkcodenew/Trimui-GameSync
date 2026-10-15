#pragma once

#include <string>

namespace GameSync {

class AppConfig {
public:
  static AppConfig &instance();
  void setDataDirectory(const std::string &path);

  // Generic key/value settings stored under the data directory.
  // Used for: drive_api_key, language, ota_enabled, intro_seen, ...
  std::string getSetting(const std::string &name,
                         const std::string &fallback = "") const;
  void setSetting(const std::string &name, const std::string &value) const;

private:
  AppConfig() = default;
  std::string readValue(const std::string &name) const;
  void writeValue(const std::string &name, const std::string &value) const;
  std::string m_dataDirectory = "/mnt/SDCARD/Apps/Trimui-GameSync/data";
};

} // namespace GameSync
