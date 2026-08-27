// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Pom1 Apple 1 Emulator
// Copyright (C) 2000-2026 Verhille Arnaud
//
// hgrpaint::decodeImageFile — PNG/JPG/BMP → RGBA via stb_image. Kept in its own
// translation unit so the pure converter (HgrConvert.cpp) stays unit-testable
// without pulling in an image decoder. The stb_image *implementation* is linked
// from the host app (main_imgui / MainWindow_Dialogs define
// STB_IMAGE_IMPLEMENTATION); here we only use its declarations.

#include "HgrConvert.h"

// Bare include so the portable hgrpaint/ toolkit doesn't hardcode POM1's tree:
// the host must put stb_image.h on the include path (POM1 adds
// src/third_party/stb in CMakeLists.txt). decl only — impl linked from the app.
#include "stb_image.h"

#include <filesystem>

namespace hgrpaint {

bool decodeImageFile(const std::string& path, int& w, int& h,
                     std::vector<uint8_t>& rgba, std::string& err)
{
    w = h = 0;
    namespace fs = std::filesystem;
    std::error_code ec;
    constexpr std::uintmax_t kMaxSourceBytes = 64u * 1024u * 1024u;
    const auto fileBytes = fs::file_size(path, ec);
    if (ec || fileBytes > kMaxSourceBytes) {
        err = "image source is not a regular file or exceeds 64 MiB";
        return false;
    }
    int channels = 0;
    if (!stbi_info(path.c_str(), &w, &h, &channels) || w <= 0 || h <= 0 ||
        w > 2048 || h > 2048 ||
        static_cast<uint64_t>(w) * static_cast<uint64_t>(h) > 4000000ull) {
        err = "image dimensions exceed the 4-megapixel import limit";
        w = h = 0;
        return false;
    }
    unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) {
        const char* why = stbi_failure_reason();
        err = std::string("cannot decode image: ") + (why ? why : "unknown format");
        return false;
    }
    rgba.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
    stbi_image_free(pixels);
    return true;
}

} // namespace hgrpaint
