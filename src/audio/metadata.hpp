#pragma once

#include "audio/audio.hpp"

#include <taglib/tpropertymap.h>
#include <taglib/tvariant.h>

namespace audio_converter {

struct Metadata {
    TagLib::PropertyMap properties;
    TagLib::List<TagLib::VariantMap> pictures;
};

[[nodiscard]] Metadata read_metadata(const fs::path& source, 
                                     const MediaTags& fallback, 
                                     std::vector<Diagnostic>& warnings);
void write_metadata(const fs::path& output, 
                    const Metadata& metadata,
                    std::vector<Diagnostic>& warnings);

} // namespace audio_converter
