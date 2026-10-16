#include "core/Settings.h"

#include <cstdio>
#include <fstream>

namespace arena::core {

bool Settings::load() {
    if (file_.empty()) return false;
    std::ifstream in(file_);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        values_[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return true;
}

bool Settings::save() const {
    if (file_.empty()) return false;
    // Write a new file and rename it over the old one, so a crash mid-write
    // never leaves half a file.
    const std::string tmp = file_ + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const auto &kv : values_) {
            out << kv.first << '=' << kv.second << '\n';
        }
        if (!out.flush()) return false;
    }
#ifdef _WIN32
    std::remove(file_.c_str());  // rename() does not replace files there
#endif
    return std::rename(tmp.c_str(), file_.c_str()) == 0;
}

std::string Settings::get(const std::string &key, const std::string &fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? fallback : it->second;
}

void Settings::set(const std::string &key, const std::string &value) {
    std::string clean;
    for (char c : value) {
        if (c != '\n' && c != '\r') clean += c;
    }
    values_[key] = clean;
}

} // namespace arena::core
