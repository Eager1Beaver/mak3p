#include "audio/metadata.hpp"

#include <taglib/fileref.h>
#include <taglib/id3v2tag.h>
#include <taglib/mpegfile.h>
#include <taglib/tfilestream.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace audio_converter {
namespace {

TagLib::String tag_string(const std::string& value) {
    return TagLib::String(value, TagLib::String::UTF8);
}

bool supported(const std::string& key) {
    static const std::set<std::string> keys{
        "TITLE", "ARTIST", "ALBUM", "ALBUMARTIST", "TRACKNUMBER", "DISCNUMBER", "GENRE",
        "DATE", "ORIGINALDATE", "COMPOSER", "COMMENT", "LYRICIST", "CONDUCTOR", "COPYRIGHT",
        "BPM", "ISRC", "LANGUAGE", "ENCODEDBY", "PUBLISHER", "TRACKTOTAL", "DISCTOTAL",
        "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK",
        "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK", "REPLAYGAIN_REFERENCE_LOUDNESS",
        "MUSICBRAINZ_TRACKID", "MUSICBRAINZ_ALBUMID", "MUSICBRAINZ_ARTISTID",
        "MUSICBRAINZ_ALBUMARTISTID", "MUSICBRAINZ_RELEASETRACKID", "MUSICBRAINZ_RELEASEGROUPID",
        "MUSICBRAINZ_WORKID", "MUSICBRAINZ_DISCID"
    };
    return keys.contains(key);
}

std::string canonical_key(std::string key) {
    for (auto& character : key) {
        const auto byte = static_cast<unsigned char>(character);
        character = static_cast<char>(std::toupper(byte));
    }
    static const std::map<std::string, std::string> aliases{
        {"ALBUM_ARTIST", "ALBUMARTIST"}, {"ALBUM ARTIST", "ALBUMARTIST"},
        {"TRACK", "TRACKNUMBER"}, {"DISC", "DISCNUMBER"}, {"YEAR", "DATE"},
        {"DESCRIPTION", "COMMENT"}, {"COMMENTS", "COMMENT"},
        {"TOTALTRACKS", "TRACKTOTAL"}, {"TOTAL_TRACKS", "TRACKTOTAL"},
        {"TOTALDISCS", "DISCTOTAL"}, {"TOTAL_DISCS", "DISCTOTAL"},
        {"WM/ALBUMARTIST", "ALBUMARTIST"}, {"WM/TRACKNUMBER", "TRACKNUMBER"}
    };
    if (const auto found = aliases.find(key); found != aliases.end()) return found->second;
    return key;
}

void append_properties(Metadata& result, 
                       const TagLib::PropertyMap& properties,
                       std::vector<Diagnostic>& warnings, 
                       std::set<std::string>& warned) {
    for (const auto& [key, values] : properties) {
        const auto name = canonical_key(key.to8Bit(true));
        if (supported(name)) {
            result.properties[tag_string(name)] = values;
        } else if (warned.insert(name).second) {
            warnings.push_back({"metadata_unmapped", {{"field", name}}, {}});
        }
    }
    for (const auto& key : properties.unsupportedData()) {
        if (key == "APIC" || key.startsWith("APIC:")) continue;
        warnings.push_back({"metadata_unsupported_data", {{"field", key.to8Bit(true)}}, {}});
    }
}

bool matching_pictures(const TagLib::List<TagLib::VariantMap>& expected,
                       const TagLib::List<TagLib::VariantMap>& actual) {
    if (expected.size() != actual.size()) return false;
    for (unsigned index = 0; index < expected.size(); ++index) {
        for (const auto& key : {"data", "mimeType", "description", "pictureType"}) {
            if (expected[index].value(key) != actual[index].value(key)) return false;
        }
    }
    return true;
}

TagLib::StringList comparable_values(const TagLib::String& key, TagLib::StringList values) {
    if (key != "DATE" && key != "ORIGINALDATE") return values;
    for (auto& value : values) {
        auto timestamp = value.to8Bit(true);
        if (timestamp.size() > 10 && timestamp[4] == '-' && timestamp[7] == '-' &&
            timestamp[10] == 'T') {
            timestamp[10] = ' ';
            value = tag_string(timestamp);
        }
    }
    return values;
}

std::optional<Diagnostic> verify(const fs::path& output, const Metadata& metadata) {
    TagLib::FileStream stream(output.c_str(), true);
    TagLib::FileRef file(&stream, false);
    if (file.isNull()) {
        return Diagnostic{"metadata_verify_failed", {{"field", "file"}}, {}};
    }
    const auto actual = file.properties();
    for (const auto& [key, values] : metadata.properties) {
        if (comparable_values(key, actual.value(key)) != comparable_values(key, values)) {
            return Diagnostic{"metadata_verify_failed", {{"field", key.to8Bit(true)}},
                "Expected: " + values.toString(" | ").to8Bit(true) +
                "\nFound: " + actual.value(key).toString(" | ").to8Bit(true)};
        }
    }
    if (actual.size() != metadata.properties.size()) {
        return Diagnostic{"metadata_verify_failed", {{"field", "unexpected text fields"}}, {}};
    }
    if (!matching_pictures(metadata.pictures, file.complexProperties("PICTURE"))) {
        return Diagnostic{"metadata_verify_failed", {{"field", "artwork"}}, {}};
    }
    return std::nullopt;
}

void save(const fs::path& output, const Metadata& metadata, TagLib::ID3v2::Version version) {
    TagLib::FileStream stream(output.c_str(), false);
    TagLib::MPEG::File file(&stream, false);
    if (!file.isValid() || file.readOnly()) throw AudioError({"metadata_save_failed", {}, {}});
    auto* tag = file.ID3v2Tag(true);
    while (!tag->frameList().isEmpty()) tag->removeFrame(tag->frameList().front());
    const auto unsupported = tag->setProperties(metadata.properties);
    if (!unsupported.isEmpty()) {
        throw AudioError({"metadata_verify_failed", {{"field", "text properties"}}, {}});
    }
    if (!tag->setComplexProperties("PICTURE", metadata.pictures) ||
        !file.save(TagLib::MPEG::File::ID3v2, TagLib::File::StripOthers,
                   version, TagLib::File::DoNotDuplicate)) {
        throw AudioError({"metadata_save_failed", {}, {}});
    }
}

} // namespace

Metadata read_metadata(const fs::path& source, 
                       const MediaTags& fallback,
                       std::vector<Diagnostic>& warnings) {
    Metadata result;
    std::set<std::string> warned;
    TagLib::FileStream stream(source.c_str(), true);
    TagLib::FileRef file(&stream, false);
    if (!file.isNull()) {
        append_properties(result, file.properties(), warnings, warned);
        for (const auto& key : file.complexPropertyKeys()) {
            if (key != "PICTURE") {
                warnings.push_back({"metadata_unsupported_data", {{"field", key.to8Bit(true)}}, {}});
            }
        }
        for (const auto& picture : file.complexProperties("PICTURE")) {
            const auto mime = picture.value("mimeType").toString().to8Bit(true);
            const auto data = picture.value("data").toByteVector();
            if ((mime != "image/jpeg" && mime != "image/png") || data.isEmpty()) {
                warnings.push_back({"metadata_picture_unsupported", {{"type", mime}}, {}});
                continue;
            }
            TagLib::VariantMap copy;
            for (const auto& key : {"data", "mimeType", "description", "pictureType"}) {
                copy.insert(key, picture.value(key));
            }
            result.pictures.append(copy);
        }
    } else {
        warnings.push_back({"metadata_unreadable", {}, {}});
    }
    for (const auto& [key, value] : fallback) {
        const auto name = canonical_key(key);
        const auto tag_key = tag_string(name);
        if (supported(name)) {
            if (!result.properties.contains(tag_key)) {
                result.properties.insert(tag_key, TagLib::StringList(tag_string(value)));
            }
        } else if (warned.insert(name).second) {
            warnings.push_back({"metadata_unmapped", {{"field", key}}, {}});
        }
    }
    for (const auto& pair : {std::pair{"TRACKNUMBER", "TRACKTOTAL"},
                            std::pair{"DISCNUMBER", "DISCTOTAL"}}) {
        auto number = result.properties.value(pair.first);
        const auto total = result.properties.value(pair.second);
        if (number.size() != 1 || total.size() != 1) continue;
        const auto position = number[0].to8Bit(true);
        const auto count = total[0].to8Bit(true);
        const auto decimal = [](const std::string& value) {
            return !value.empty() && std::all_of(value.begin(), value.end(),
                [](unsigned char byte) { return byte >= '0' && byte <= '9'; });
        };
        if (decimal(position) && decimal(count)) {
            result.properties.replace(pair.first, TagLib::StringList(tag_string(position + "/" + count)));
            result.properties.erase(pair.second);
        } else if (const auto slash = position.find('/');
                   slash != std::string::npos && position.substr(slash + 1) == count) {
            result.properties.erase(pair.second);
        }
    }
    return result;
}

void write_metadata(const fs::path& output, const Metadata& metadata,
                    std::vector<Diagnostic>& warnings) {
    save(output, metadata, TagLib::ID3v2::v3);
    if (!verify(output, metadata)) return;
    save(output, metadata, TagLib::ID3v2::v4);
    if (const auto error = verify(output, metadata)) {
        throw AudioError(*error);
    }
    warnings.push_back({"metadata_v4", {}, {}});
}

} // namespace audio_converter
