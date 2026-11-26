// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    // Drops everything cached so far, so the cache starts over empty. Call before FinishPreload.
    void Clear();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);

private:
    /// The loose files read into memory at Open: from the pack file and the files written since
    /// it was made (build 51).
    struct PackedBlob {
        const u8* data{};
        u32 size{};
    };
    void OpenPack();
    void WritePack();
    bool LoadPacked(const std::string& file_name, std::vector<u8>& data);

    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::filesystem::path pack_path{};
    bool opened{};
    /// Recursive: loading a pipeline from ForEachBlob loads its shaders by name.
    std::recursive_mutex pack_mutex{};
    bool pack_active{};
    bool pack_dirty{};
    std::vector<u8> pack_bytes{};
    std::deque<std::vector<u8>> loose_bytes{};
    std::unordered_map<std::string, PackedBlob> packed{};
};

} // namespace Storage
