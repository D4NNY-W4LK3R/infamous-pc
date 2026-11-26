// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <mutex>
#include <queue>

namespace {

std::mutex submit_mutex{};
u32 num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

} // namespace

namespace Storage {

std::string GetBlobFileExtensionString(BlobType type);

namespace {

/// The file name a blob has in the folder, as LoadVector makes it.
std::string PackedName(BlobType type, const std::string& name) {
    return std::filesystem::path{name}
        .replace_extension(GetBlobFileExtensionString(type))
        .filename()
        .string();
}

/// A cache this large takes longer to preload than rebuilding it in game costs, so it starts over.
constexpr u64 MaxCacheSize = 2_GB;

u64 DirectorySize(const std::filesystem::path& path) {
    std::error_code ec;
    u64 size{};
    std::filesystem::directory_iterator it{path, ec};
    while (!ec && it != std::filesystem::directory_iterator{}) {
        if (it->is_regular_file(ec)) {
            size += it->file_size(ec);
        }
        it.increment(ec);
    }
    return size;
}

} // namespace

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        std::packaged_task<void()> request{};
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests; });
            if (stoken.stop_requested()) {
                break;
            }
            {
                std::scoped_lock lock{m_request};
                if (!req_queue.empty()) {
                    request = std::move(req_queue.front());
                    req_queue.pop();
                }
            }
            if (num_requests > 0) {
                --num_requests;
            }
        }

        if (request.valid()) {
            try {
                request();
            } catch (...) {
            }
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

std::string GetBlobFileExtensionString(BlobType type) {
    return GetBlobFileExtension(type);
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        cache_path = GetUserPath(PathType::CacheDir) / game_info.GameSerial();
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        pack_path = GetUserPath(PathType::CacheDir) /
                    std::filesystem::path{game_info.GameSerial()}.replace_extension(".pack");
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
    if (!EmulatorSettings.IsPipelineCacheArchived()) {
        OpenPack();
    }

    std::error_code ec;
    const u64 size = EmulatorSettings.IsPipelineCacheArchived()
                         ? std::filesystem::file_size(cache_path, ec)
                         : DirectorySize(cache_path);
    if (!ec && size > MaxCacheSize) {
        LOG_INFO(Render, "Pipeline cache grew to {} MB, starting it over", size >> 20);
        Clear();
    }
}

void DataBase::Clear() {
    if (!IsOpened()) {
        return;
    }

    std::error_code ec;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_reader_end(&zip_ar);
        std::filesystem::remove(cache_path, ec);
        mz_zip_zero_struct(&zip_ar);
        mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
    } else {
        std::filesystem::remove_all(cache_path, ec);
        std::filesystem::create_directories(cache_path, ec);
        std::error_code pack_ec;
        std::filesystem::remove(pack_path, pack_ec);
        std::scoped_lock lock{pack_mutex};
        pack_active = false;
        pack_dirty = false;
        packed.clear();
        loose_bytes.clear();
        std::vector<u8>{}.swap(pack_bytes);
    }
    if (ec) {
        LOG_WARNING(Render, "Failed to clear the cache at {}: {}", cache_path.string(),
                    ec.message());
    }
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                const auto file = IOFile{path, FileAccessMode::Create};
                file.Write(v);
            }
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }
    if (pack_active && LoadPacked(PackedName(type, name), data)) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }
    if (pack_active) {
        std::vector<u8> bytes;
        if (LoadPacked(PackedName(type, name), bytes)) {
            data.resize(bytes.size() / sizeof(u32));
            std::memcpy(data.data(), bytes.data(), data.size() * sizeof(u32));
            return;
        }
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else if (pack_active) {
        std::scoped_lock lock{pack_mutex};
        for (const auto& [file_name, blob] : packed) {
            if (file_name.ends_with(ext) && file_name.size() > ext.size() &&
                file_name[file_name.size() - ext.size() - 1] == '.') {
                func(std::vector<u8>(blob.data, blob.data + blob.size));
            }
        }
    } else {
        std::error_code ec;
        std::filesystem::directory_iterator it{cache_path, ec};
        while (!ec && it != std::filesystem::directory_iterator{}) {
            if (it->path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{it->path(), FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
            it.increment(ec);
        }
    }
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
        return;
    }
    if (!pack_active) {
        return;
    }
    // The pack is rewritten when files were read loose (new since it was made), on the cache's
    // own thread so the game starts meanwhile; the memory goes once it is written or was current.
    {
        std::scoped_lock lock{m_request};
        req_queue.emplace([this] { WritePack(); });
    }
    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
}

namespace {

constexpr u32 PackMagic = 0x4B505353; // "SSPK"
constexpr u32 PackVersion = 1;

bool PackDisabled() {
    const char* env = std::getenv("SS_SHADER_PACK");
    return env && env[0] == '0';
}

} // namespace

void DataBase::OpenPack() {
    // The shader database is about 35,000 small files: opening each on a hard disk took 392 s of
    // a 409 s start (build 50). They are also kept in one file, read in one go; only the files
    // written since it was made are opened one by one (build 51).
    if (PackDisabled()) {
        LOG_INFO(Render, "Startup: shader pack off (SS_SHADER_PACK=0), reading loose files");
        return;
    }
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    std::scoped_lock lock{pack_mutex};
    pack_active = true;
    packed.clear();
    loose_bytes.clear();
    std::vector<u8>{}.swap(pack_bytes);

    u32 from_pack = 0;
    std::error_code time_ec;
    const auto pack_time = std::filesystem::last_write_time(pack_path, time_ec);
    {
        using namespace Common::FS;
        const IOFile file{pack_path, FileAccessMode::Read};
        if (file.IsOpen()) {
            pack_bytes.resize(file.GetSize());
            if (file.Read(pack_bytes) != pack_bytes.size()) {
                pack_bytes.clear();
            }
        }
    }
    const auto read_done = Clock::now();
    if (pack_bytes.size() >= 12) {
        u32 header[3];
        std::memcpy(header, pack_bytes.data(), sizeof(header));
        size_t pos = sizeof(header);
        bool valid = header[0] == PackMagic && header[1] == PackVersion;
        for (u32 i = 0; valid && i < header[2]; ++i) {
            u16 name_len{};
            u32 size{};
            if (pos + sizeof(name_len) > pack_bytes.size()) {
                valid = false;
                break;
            }
            std::memcpy(&name_len, pack_bytes.data() + pos, sizeof(name_len));
            pos += sizeof(name_len);
            if (pos + name_len + sizeof(size) > pack_bytes.size()) {
                valid = false;
                break;
            }
            std::string name{reinterpret_cast<const char*>(pack_bytes.data() + pos), name_len};
            pos += name_len;
            std::memcpy(&size, pack_bytes.data() + pos, sizeof(size));
            pos += sizeof(size);
            if (pos + size > pack_bytes.size()) {
                valid = false;
                break;
            }
            packed[std::move(name)] = PackedBlob{pack_bytes.data() + pos, size};
            pos += size;
        }
        if (!valid) {
            LOG_WARNING(Render, "Startup: shader pack {} is damaged, reading loose files",
                        pack_path.string());
            packed.clear();
            std::vector<u8>{}.swap(pack_bytes);
        }
        from_pack = static_cast<u32>(packed.size());
    }

    // Listing the folder doesn't open its files: only names and sizes the pack lacks are read.
    u32 loose = 0;
    std::error_code ec;
    std::filesystem::directory_iterator it{cache_path, ec};
    while (!ec && it != std::filesystem::directory_iterator{}) {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec)) {
            const std::string name = it->path().filename().string();
            const u64 size = it->file_size(entry_ec);
            std::error_code write_ec;
            const bool newer = time_ec || it->last_write_time(write_ec) > pack_time || write_ec;
            const auto found = packed.find(name);
            if (!entry_ec && (found == packed.end() || found->second.size != size || newer)) {
                using namespace Common::FS;
                const IOFile file{it->path(), FileAccessMode::Read};
                if (file.IsOpen()) {
                    auto& bytes = loose_bytes.emplace_back(file.GetSize());
                    file.Read(bytes);
                    packed[name] = PackedBlob{bytes.data(), static_cast<u32>(bytes.size())};
                    ++loose;
                }
            }
        }
        it.increment(ec);
    }
    pack_dirty = loose > 0;
    LOG_INFO(Render,
             "Startup: shader pack: {} files from {} ({} MB, read in {:.1f} s), {} loose files "
             "read that it lacks, {:.1f} s in all",
             from_pack, pack_path.filename().string(), pack_bytes.size() >> 20,
             std::chrono::duration<double>(read_done - start).count(), loose,
             std::chrono::duration<double>(Clock::now() - start).count());
}

bool DataBase::LoadPacked(const std::string& file_name, std::vector<u8>& data) {
    std::scoped_lock lock{pack_mutex};
    if (!pack_active) {
        return false;
    }
    const auto it = packed.find(file_name);
    if (it == packed.end()) {
        return false;
    }
    data.assign(it->second.data, it->second.data + it->second.size);
    return true;
}

void DataBase::WritePack() {
    std::scoped_lock lock{pack_mutex};
    if (!pack_active) {
        return;
    }
    if (pack_dirty && !packed.empty()) {
        const auto start = std::chrono::steady_clock::now();
        auto temp = pack_path;
        temp += ".tmp";
        bool ok = false;
        u64 bytes_written = 0;
        {
            using namespace Common::FS;
            IOFile file{temp, FileAccessMode::Create};
            if (file.IsOpen()) {
                std::vector<u8> out;
                out.reserve(16_MB);
                const auto put = [&](const void* src, size_t n) {
                    const auto* p = static_cast<const u8*>(src);
                    out.insert(out.end(), p, p + n);
                };
                const u32 header[3] = {PackMagic, PackVersion, static_cast<u32>(packed.size())};
                put(header, sizeof(header));
                ok = true;
                for (const auto& [name, blob] : packed) {
                    const u16 name_len = static_cast<u16>(name.size());
                    put(&name_len, sizeof(name_len));
                    put(name.data(), name.size());
                    put(&blob.size, sizeof(blob.size));
                    put(blob.data, blob.size);
                    if (out.size() >= 16_MB) {
                        ok = ok && file.Write(out) == out.size();
                        bytes_written += out.size();
                        out.clear();
                    }
                }
                ok = ok && file.Write(out) == out.size();
                bytes_written += out.size();
                ok = ok && file.Flush();
            }
        }
        std::error_code ec;
        if (ok) {
            std::filesystem::rename(temp, pack_path, ec);
        }
        if (!ok || ec) {
            std::filesystem::remove(temp, ec);
            LOG_WARNING(Render, "Shader pack could not be written; the next start reads the "
                                "loose files again");
        } else {
            LOG_INFO(
                Render, "Shader pack written: {} files, {} MB in {:.1f} s", packed.size(),
                bytes_written >> 20,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
    }
    // The loose files stay as they are: they are the database, the pack only reads it faster.
    pack_active = false;
    pack_dirty = false;
    packed.clear();
    loose_bytes.clear();
    std::vector<u8>{}.swap(pack_bytes);
}

} // namespace Storage
