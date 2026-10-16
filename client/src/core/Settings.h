// A few remembered settings (the last server, the player's name) in a small
// "key=value" text file. Used where there is no command line (Android).
#pragma once

#include <map>
#include <string>

namespace arena::core {

class Settings {
public:
    explicit Settings(std::string file) : file_(std::move(file)) {}

    bool load();        // false if there is no file (yet)
    bool save() const;  // false if it could not be written (or no file)

    std::string get(const std::string &key, const std::string &fallback = {}) const;
    void set(const std::string &key, const std::string &value);

private:
    std::string file_;
    std::map<std::string, std::string> values_;
};

} // namespace arena::core
