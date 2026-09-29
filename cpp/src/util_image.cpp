// stb_image : decodeur PNG/JPEG embarque (licence publique, une seule fois).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "util_image.hpp"

#include "miniz.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <SDL_opengl.h>

#include <fstream>
#include <vector>

namespace tl::image {
namespace {

void upload(unsigned tex, const unsigned char* px, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, px);
}

} // namespace

unsigned from_mem(const void* data, int size, int* ow, int* oh) {
    if (ow) *ow = 0;
    if (oh) *oh = 0;
    if (!data || size <= 0) return 0;
    int w = 0, h = 0, comp = 0;
    unsigned char* px =
        stbi_load_from_memory(static_cast<const stbi_uc*>(data), size, &w, &h,
                              &comp, 4);
    if (!px) return 0;
    unsigned tex = 0;
    glGenTextures(1, &tex);
    upload(tex, px, w, h);
    stbi_image_free(px);
    if (ow) *ow = w;
    if (oh) *oh = h;
    return tex;
}

unsigned from_path(const std::filesystem::path& p, int* w, int* h) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return 0;
    std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (buf.empty()) return 0;
    return from_mem(buf.data(), static_cast<int>(buf.size()), w, h);
}

std::vector<unsigned char> decode_rgba(const std::filesystem::path& p, int* ow,
                                       int* oh) {
    if (ow) *ow = 0;
    if (oh) *oh = 0;
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    const std::vector<char> raw((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    if (raw.empty()) return {};

    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc*>(raw.data()),
        static_cast<int>(raw.size()), &w, &h, &comp, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return {};
    }
    std::vector<unsigned char> out(px, px + static_cast<std::size_t>(w) * h * 4);
    stbi_image_free(px);
    if (ow) *ow = w;
    if (oh) *oh = h;
    return out;
}

void free_tex(unsigned tex) {
    if (tex) glDeleteTextures(1, &tex);
}

bool write_png(const std::filesystem::path& dest, int w, int h, int comp,
               const void* data, bool flipY) {
    if (!data || w <= 0 || h <= 0 || (comp != 3 && comp != 4)) return false;
    std::size_t len = 0;
    // Encodeur PNG de miniz (deja lie pour les zip) : evite d'embarquer
    // stb_image_write juste pour les captures.
    void* png = tdefl_write_image_to_png_file_in_memory_ex(
        data, w, h, comp, &len, /*level=*/6, flipY ? MZ_TRUE : MZ_FALSE);
    if (!png) return false;

    std::error_code ec;
    if (dest.has_parent_path()) std::filesystem::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    bool ok = false;
    if (out) {
        out.write(static_cast<const char*>(png), static_cast<std::streamsize>(len));
        ok = static_cast<bool>(out);
    }
    mz_free(png);
    return ok;
}

} // namespace tl::image
