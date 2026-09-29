#include "util_zip.hpp"

#include "miniz.h"

#include <cctype>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace tl {

namespace {
bool has_traversal(const std::string& name) {
    return name.find("..") != std::string::npos ||
           (!name.empty() && (name[0] == '/' || name[0] == '\\'));
}
} // namespace

int zip_extract_natives(const fs::path& jar, const fs::path& destDir) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, jar.string().c_str(), 0))
        return -1;
    std::error_code ec;
    fs::create_directories(destDir, ec);

#ifdef _WIN32
    constexpr const char* kNativeExt = ".dll";
#else
    constexpr const char* kNativeExt = ".so";
#endif
    const size_t extLen = std::char_traits<char>::length(kNativeExt);
    int extracted = 0;
    const int count = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        const std::string name = st.m_filename;
        if (name.size() < extLen) continue;
        // finit par l'extension native (insensible a la casse), sans traversal
        const std::string tail = name.substr(name.size() - extLen);
        bool match = tail.size() == extLen;
        for (size_t k = 0; match && k < extLen; ++k)
            match = std::tolower(static_cast<unsigned char>(tail[k])) ==
                    static_cast<unsigned char>(kNativeExt[k]);
        if (!match) continue;
        if (has_traversal(name)) continue;

        fs::path dest = destDir / fs::path(name);
        if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
        if (!mz_zip_reader_extract_to_file(&zip, i, dest.string().c_str(), 0))
            continue;
        ++extracted;
    }
    mz_zip_reader_end(&zip);
    return extracted;
}

bool zip_extract_entry(const fs::path& zipPath, const std::string& entryName,
                       const fs::path& destPath) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.string().c_str(), 0))
        return false;
    bool ok = false;
    const int idx = mz_zip_reader_locate_file(&zip, entryName.c_str(), nullptr, 0);
    if (idx >= 0) {
        std::error_code ec;
        if (destPath.has_parent_path()) fs::create_directories(destPath.parent_path(), ec);
        ok = mz_zip_reader_extract_to_file(&zip, idx, destPath.string().c_str(), 0);
    }
    mz_zip_reader_end(&zip);
    return ok;
}

std::optional<std::string> zip_read_entry(const fs::path& zipPath,
                                          const std::string& entryName) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.string().c_str(), 0))
        return std::nullopt;
    std::optional<std::string> out;
    const int idx = mz_zip_reader_locate_file(&zip, entryName.c_str(), nullptr, 0);
    if (idx >= 0) {
        size_t size = 0;
        if (void* mem = mz_zip_reader_extract_to_heap(&zip, idx, &size, 0)) {
            out = std::string(static_cast<const char*>(mem), size);
            mz_free(mem);
        }
    }
    mz_zip_reader_end(&zip);
    return out;
}

int zip_extract_prefix(const fs::path& zipPath, const std::string& prefix,
                       const fs::path& destDir) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.string().c_str(), 0))
        return -1;
    std::error_code ec;
    fs::create_directories(destDir, ec);

    int written = 0;
    const int count = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        const std::string name = st.m_filename;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rel = name.substr(prefix.size());
        if (rel.empty() || rel.back() == '/' || rel.back() == '\\') continue; // dossier
        if (has_traversal(rel)) continue;

        fs::path dest = destDir / fs::path(rel);
        if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
        if (mz_zip_reader_extract_to_file(&zip, i, dest.string().c_str(), 0))
            ++written;
    }
    mz_zip_reader_end(&zip);
    return written;
}

int zip_extract_all(const fs::path& zipPath, const fs::path& destDir) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, zipPath.string().c_str(), 0))
        return -1;
    std::error_code ec;
    fs::create_directories(destDir, ec);

    int written = 0;
    const int count = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        const std::string name = st.m_filename;
        if (name.empty() || name.back() == '/' || name.back() == '\\') continue; // dossier
        if (has_traversal(name)) continue;

        fs::path dest = destDir / fs::path(name);
        if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
        if (mz_zip_reader_extract_to_file(&zip, i, dest.string().c_str(), 0))
            ++written;
    }
    mz_zip_reader_end(&zip);
    return written;
}

bool zip_create_from_dir(const fs::path& dir, const fs::path& zipPath) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    std::error_code ec2;
    fs::remove(zipPath, ec2); // ecrase une eventuelle archive precedente

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, zipPath.string().c_str(), 0))
        return false;

    int added = 0;
    fs::recursive_directory_iterator it(dir, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const fs::path p = it->path();
        const std::string rel = fs::relative(p, dir, ec).generic_string();
        if (ec || rel.empty() || rel.find("..") != std::string::npos) continue;
        if (mz_zip_writer_add_file(&zip, rel.c_str(),
                                   p.string().c_str(), nullptr, 0,
                                   MZ_DEFAULT_COMPRESSION))
            ++added;
    }

    const bool ok = mz_zip_writer_finalize_archive(&zip) != 0 && added > 0;
    mz_zip_writer_end(&zip);
    if (!ok) {
        fs::remove(zipPath, ec2);
        return false;
    }
    return true;
}

} // namespace tl
