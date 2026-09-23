#pragma once

// PNG -> drawable texture, done at IMPORT time so the reader never decodes on
// the draw path. See image.cpp for why GX_TF_RGBA8_PC and a single allocation.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace dusk::guide {

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // linear RGBA8, tightly packed
};

// Decodes PNG and JPEG (stb_image). Anything else fails and the reader falls
// back to the alt text.
bool decode_png_to_rgba(const std::string& bytes, RgbaImage& out);

// Wraps linear RGBA8 in a ResTIMG header. Header and pixels share one buffer
// because ResTIMG::imageOffset is a delta from the header itself.
std::vector<std::uint8_t> build_timg(const RgbaImage& img);

// Reads just the dimensions of an already-stored image, in the SAME units
// store_image_file reports (i.e. after the import downscale), without decoding
// the pixels. Used when a re-import finds the image already on disk.
bool probe_image_size(const std::filesystem::path& file, int* o_width, int* o_height);

// Validates and stores an image for later display. Keeps the compressed source
// bytes, not the decoded texture: a 512x512 RGBA8 blob is 1 MB versus ~40 KB as
// JPEG, and the decode at load time is bounded by the reader's RAM cache.
bool store_image_file(const std::string& bytes, const std::filesystem::path& outFile,
    int* o_width = nullptr, int* o_height = nullptr);

// Reads a stored image and decodes it into a drawable ResTIMG blob. The
// returned bytes must outlive any J2DPicture built over them — imageOffset
// points inside this buffer. Empty on failure.
std::vector<std::uint8_t> load_timg_blob(const std::filesystem::path& file);

}  // namespace dusk::guide
