#pragma once

// Decodage PNG/JPEG (stb_image) + texture OpenGL (backend GL3 d'ImGui).
// Creation/liberation sur le main thread uniquement (contexte GL courant).

#include <filesystem>

namespace tl::image {

// Decode en RGBA + upload GL. Retourne 0 si echec. w/h optionnels.
unsigned from_mem(const void* data, int size, int* w = nullptr,
                  int* h = nullptr);
unsigned from_path(const std::filesystem::path& p, int* w = nullptr,
                   int* h = nullptr);
void free_tex(unsigned tex);

} // namespace tl::image
