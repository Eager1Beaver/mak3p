#include "app/import.hpp"
#include "audio/process.hpp"

#include <algorithm>
#include <cctype>

namespace audio_converter {
fs::path path_from_utf8(std::string_view text) {
    if (text.empty() || text.find('\0') != text.npos) throw AudioError({"gui_bad_drop", {}, {}});
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

std::vector<fs::path> dropped_paths(std::string_view payload) {
    std::vector<fs::path> result;
    while (!payload.empty()) {
        const auto end = payload.find('\n');
        auto line = payload.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty() && !line.starts_with('#')) {
            std::string decoded;
            if (line.starts_with("file://")) {
                line.remove_prefix(7);
                if (line.starts_with("localhost/")) line.remove_prefix(9);
                if (!line.starts_with('/')) throw AudioError({"gui_bad_drop", {}, {}});
                for (std::size_t i = 0; i < line.size(); ++i) {
                    if (line[i] != '%') { decoded += line[i]; continue; }
                    if (i + 2 >= line.size()) throw AudioError({"gui_bad_drop", {}, {}});
                    auto hex = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    const int high = hex(line[i + 1]), low = hex(line[i + 2]);
                    if (high < 0 || low < 0) throw AudioError({"gui_bad_drop", {}, {}});
                    decoded += static_cast<char>(high * 16 + low); i += 2;
                }
#ifdef _WIN32
                if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':') decoded.erase(0, 1);
#endif
            } else {
                if (line.find("://") != line.npos) throw AudioError({"gui_bad_drop", {}, {}});
                decoded = line;
            }
            result.push_back(path_from_utf8(decoded));
        }
        if (end == payload.npos) break;
        payload.remove_prefix(end + 1);
    }
    if (result.empty()) throw AudioError({"gui_bad_drop", {}, {}});
    return result;
}

} // namespace audio_converter
