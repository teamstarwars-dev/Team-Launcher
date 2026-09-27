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

// Ecrit une image brute en PNG (encodeur de miniz, aucune dependance de plus).
// comp : 3 = RGB, 4 = RGBA. `flipY` retourne verticalement — glReadPixels rend
// l'image tete en bas par rapport a l'orientation PNG.
bool write_png(const std::filesystem::path& dest, int w, int h, int comp,
               const void* data, bool flipY = false);

} // namespace tl::image
