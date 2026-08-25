#include <windows.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>

#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>
#include "MediaLibrary.h"
#include "ThumbnailGenerator.h"

namespace fs = std::filesystem;

namespace
{
    constexpr uint64_t maximumDecodedPixels = 100'000'000;

    class bcrypt_algorithm
    {
    public:
        ~bcrypt_algorithm() { if (value_) BCryptCloseAlgorithmProvider(value_, 0); }
        bcrypt_algorithm(bcrypt_algorithm const&) = delete;
        bcrypt_algorithm& operator=(bcrypt_algorithm const&) = delete;
        bcrypt_algorithm() = default;
        BCRYPT_ALG_HANDLE get() const noexcept { return value_; }
        BCRYPT_ALG_HANDLE* put() noexcept { return &value_; }
    private:
        BCRYPT_ALG_HANDLE value_{};
    };

    class bcrypt_hash
    {
    public:
        ~bcrypt_hash() { if (value_) BCryptDestroyHash(value_); }
        bcrypt_hash(bcrypt_hash const&) = delete;
        bcrypt_hash& operator=(bcrypt_hash const&) = delete;
        bcrypt_hash() = default;
        BCRYPT_HASH_HANDLE get() const noexcept { return value_; }
        BCRYPT_HASH_HANDLE* put() noexcept { return &value_; }
    private:
        BCRYPT_HASH_HANDLE value_{};
    };

    std::wstring trim(std::wstring value)
    {
        auto notSpace = [](wchar_t character) { return !iswspace(character); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    std::wstring copy_and_sha256(fs::path const& source, fs::path const& destination,
        motion::app::MediaLibrary::ImportProgress const& progress, std::atomic_bool const* cancelled)
    {
        bcrypt_algorithm algorithm;
        bcrypt_hash hash;
        DWORD objectLength{}, resultLength{};
        winrt::check_nt(BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        winrt::check_nt(BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &resultLength, 0));
        std::vector<uint8_t> object(objectLength);
        std::array<uint8_t, 32> digest{};
        winrt::check_nt(BCryptCreateHash(algorithm.get(), hash.put(), object.data(),
            static_cast<ULONG>(object.size()), nullptr, 0, 0));
        std::ifstream input(source, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open media for hashing");
        std::ofstream destinationStream(destination, std::ios::binary | std::ios::trunc);
        if (!destinationStream) throw std::runtime_error("cannot create imported media");
        uint64_t total = fs::file_size(source);
        uint64_t copied{};
        std::vector<uint8_t> buffer(4 * 1024 * 1024);
        while (input) {
            if (cancelled && cancelled->load(std::memory_order_relaxed)) throw std::runtime_error("import cancelled");
            input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
            auto count = input.gcount();
            if (count > 0) {
                winrt::check_nt(BCryptHashData(hash.get(), buffer.data(), static_cast<ULONG>(count), 0));
                destinationStream.write(reinterpret_cast<char*>(buffer.data()), count);
                if (!destinationStream) throw std::runtime_error("cannot write imported media");
                copied += static_cast<uint64_t>(count);
                if (progress) progress(copied, total);
            }
        }
        if (!input.eof()) throw std::runtime_error("cannot read imported media");
        destinationStream.flush();
        if (!destinationStream) throw std::runtime_error("cannot flush imported media");
        winrt::check_nt(BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0));
        std::wostringstream hexDigest;
        hexDigest << std::hex << std::setfill(L'0');
        for (auto byte : digest) hexDigest << std::setw(2) << static_cast<unsigned>(byte);
        return hexDigest.str();
    }

    void validate_image(fs::path const& source)
    {
        winrt::com_ptr<IWICImagingFactory> factory;
        winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory2), factory.put_void()));
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::check_hresult(factory->CreateDecoderFromFilename(source.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, decoder.put()));
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(0, frame.put()));
        UINT width{}, height{};
        winrt::check_hresult(frame->GetSize(&width, &height));
        if (!width || !height || static_cast<uint64_t>(width) * height > maximumDecodedPixels) {
            throw std::runtime_error("image dimensions exceed safety limit");
        }
    }

    void validate_video(fs::path const& source)
    {
        winrt::check_hresult(MFStartup(MF_VERSION, MFSTARTUP_FULL));
        try {
            winrt::com_ptr<IMFSourceReader> reader;
            winrt::check_hresult(MFCreateSourceReaderFromURL(source.c_str(), nullptr, reader.put()));
            winrt::com_ptr<IMFMediaType> mediaType;
            winrt::check_hresult(reader->GetNativeMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, mediaType.put()));
            UINT32 width{}, height{};
            winrt::check_hresult(MFGetAttributeSize(mediaType.get(), MF_MT_FRAME_SIZE, &width, &height));
            if (!width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
                static_cast<uint64_t>(width) * height > maximumDecodedPixels) {
                throw std::runtime_error("video dimensions exceed safety limit");
            }
            MFShutdown();
        } catch (...) {
            MFShutdown();
            throw;
        }
    }

    void require_import_space(fs::path const& destinationRoot, fs::path const& source)
    {
        auto sourceSize = fs::file_size(source);
        ULARGE_INTEGER available{};
        if (!GetDiskFreeSpaceExW(destinationRoot.c_str(), &available, nullptr, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
        }
        constexpr uint64_t minimumReserve = 256ULL * 1024 * 1024;
        uint64_t required = sourceSize > UINT64_MAX - minimumReserve ? UINT64_MAX : sourceSize + minimumReserve;
        if (available.QuadPart < required) throw std::runtime_error("insufficient disk space for import");
    }
}

namespace motion::app
{
    MediaLibrary::MediaLibrary(fs::path root, DeleteMode deleteMode) : root_(std::move(root)), deleteMode_(deleteMode) {}

    void MediaLibrary::EnsureDirectories() const
    {
        fs::create_directories(WallpapersPath() / L"Groups");
        fs::create_directories(root_ / L"Config");
    }

    fs::path MediaLibrary::WallpapersPath() const { return root_ / L"Wallpapers"; }

    GroupLoadResult MediaLibrary::LoadGroups()
    {
        std::scoped_lock lock(mutex_);
        GroupLoadResult result;
        auto groupsRoot = WallpapersPath() / L"Groups";
        for (auto const& entry : fs::directory_iterator(groupsRoot)) {
            if (!entry.is_directory()) continue;
            try {
                if (auto group = motion::load_group(entry.path() / L"group.json")) result.groups.push_back(std::move(*group));
            } catch (...) {}
        }
        std::stable_sort(result.groups.begin(), result.groups.end(), [](auto const& left, auto const& right) { return left.order < right.order; });

        if (result.groups.empty()) {
            motion::GroupMetadata group;
            group.id = motion::new_id();
            group.name = L"我的壁纸";
            group.createdAt = group.updatedAt = motion::timestamp_utc();
            auto directory = groupsRoot / motion::utf8_to_wide(group.id);
            fs::create_directories(directory / L"Videos");
            motion::save_group(directory / L"group.json", group);
            result.groups.push_back(std::move(group));
        }
        return result;
    }

    std::vector<motion::MediaMetadata> MediaLibrary::LoadMedia(std::string const& groupId)
    {
        std::scoped_lock lock(mutex_);
        std::vector<motion::MediaMetadata> result;
        if (!motion::valid_id(groupId)) return result;
        auto directory = WallpapersPath() / L"Groups" / motion::utf8_to_wide(groupId) / L"Videos";
        fs::create_directories(directory);
        for (auto const& entry : fs::directory_iterator(directory)) {
            if (!entry.is_directory()) continue;
            try {
                auto media = motion::load_media(entry.path() / L"metadata.json");
                if (!media) continue;
                auto directoryId = motion::wide_to_utf8(entry.path().filename().wstring());
                if (!motion::valid_id(directoryId) || media->id != directoryId) continue;
                if (media->groupId != groupId) {
                    media->groupId = groupId;
                    ++media->revision;
                    media->updatedAt = motion::timestamp_utc();
                    motion::save_media(entry.path() / L"metadata.json", *media);
                }
                if (media->kind == "image" && media->coverFileName.empty()) media->coverFileName = media->fileName;
                result.push_back(std::move(*media));
            } catch (...) {}
        }
        return result;
    }

    motion::GroupMetadata MediaLibrary::CreateGroup(std::wstring const& value, std::vector<motion::GroupMetadata> const& existing)
    {
        std::scoped_lock lock(mutex_);
        auto name = trim(value);
        if (name.empty()) throw std::runtime_error("group name is empty");
        for (auto const& group : existing) if (_wcsicmp(group.name.c_str(), name.c_str()) == 0) throw std::runtime_error("group already exists");
        motion::GroupMetadata group;
        group.id = motion::new_id();
        group.name = std::move(name);
        for (auto const& item : existing) group.order = (std::max)(group.order, item.order + 1);
        group.createdAt = group.updatedAt = motion::timestamp_utc();
        auto directory = WallpapersPath() / L"Groups" / motion::utf8_to_wide(group.id);
        fs::create_directories(directory / L"Videos");
        motion::save_group(directory / L"group.json", group);
        return group;
    }

    void MediaLibrary::RenameGroup(motion::GroupMetadata const& group, std::wstring const& value,
        std::vector<motion::GroupMetadata> const& existing)
    {
        std::scoped_lock lock(mutex_);
        auto name = trim(value);
        if (name.empty()) throw std::runtime_error("group name is empty");
        for (auto const& item : existing) {
            if (item.id != group.id && _wcsicmp(item.name.c_str(), name.c_str()) == 0) throw std::runtime_error("group already exists");
        }
        auto path = WallpapersPath() / L"Groups" / motion::utf8_to_wide(group.id) / L"group.json";
        auto current = motion::load_group(path);
        if (!current || current->id != group.id) throw std::runtime_error("group changed during rename");
        auto updated = std::move(*current);
        updated.name = std::move(name);
        updated.updatedAt = motion::timestamp_utc();
        motion::save_group(path, updated);
    }

    void MediaLibrary::ReorderGroup(std::string const& groupId, int direction, std::vector<motion::GroupMetadata> const& groups)
    {
        auto found = std::find_if(groups.begin(), groups.end(), [&](auto const& group) { return group.id == groupId; });
        if (found == groups.end() || !direction) return;
        auto index = static_cast<std::ptrdiff_t>(std::distance(groups.begin(), found));
        auto target = index + (direction < 0 ? -1 : 1);
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(groups.size())) return;

        std::vector<std::string> orderedIds;
        orderedIds.reserve(groups.size());
        for (auto const& group : groups) orderedIds.push_back(group.id);
        std::swap(orderedIds[static_cast<size_t>(index)], orderedIds[static_cast<size_t>(target)]);
        SetGroupOrder(orderedIds, groups);
    }

    void MediaLibrary::SetGroupOrder(std::vector<std::string> const& orderedIds, std::vector<motion::GroupMetadata> const& groups)
    {
        std::scoped_lock lock(mutex_);
        if (orderedIds.size() != groups.size()) throw std::runtime_error("invalid group order");
        auto root = WallpapersPath() / L"Groups";
        auto timestamp = motion::timestamp_utc();
        std::vector<std::string> visited;
        std::vector<motion::GroupMetadata> originals;
        std::vector<motion::GroupMetadata> updatedGroups;
        visited.reserve(orderedIds.size());
        originals.reserve(orderedIds.size());
        updatedGroups.reserve(orderedIds.size());

        for (size_t index = 0; index < orderedIds.size(); ++index) {
            auto const& id = orderedIds[index];
            if (std::find(visited.begin(), visited.end(), id) != visited.end()) throw std::runtime_error("duplicate group id");
            auto found = std::find_if(groups.begin(), groups.end(), [&](auto const& group) { return group.id == id; });
            if (found == groups.end()) throw std::runtime_error("unknown group id");
            visited.push_back(id);

            auto path = root / motion::utf8_to_wide(id) / L"group.json";
            auto current = motion::load_group(path);
            if (!current || current->id != id) throw std::runtime_error("group changed during reorder");
            originals.push_back(*current);
            auto updated = std::move(*current);
            updated.order = static_cast<int>(index);
            updated.updatedAt = timestamp;
            updatedGroups.push_back(std::move(updated));
        }

        std::vector<fs::path> staged;
        staged.reserve(updatedGroups.size());
        try {
            for (auto const& updated : updatedGroups) {
                auto pending = root / motion::utf8_to_wide(updated.id) / L"group.order.pending.json";
                motion::save_group(pending, updated);
                staged.push_back(std::move(pending));
            }
            for (size_t index = 0; index < updatedGroups.size(); ++index) {
                auto destination = root / motion::utf8_to_wide(updatedGroups[index].id) / L"group.json";
                if (!MoveFileExW(staged[index].c_str(), destination.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
                }
            }
        } catch (...) {
            for (auto const& pending : staged) {
                std::error_code ignored;
                fs::remove(pending, ignored);
            }
            for (auto const& original : originals) {
                try {
                    motion::save_group(root / motion::utf8_to_wide(original.id) / L"group.json", original);
                } catch (...) {}
            }
            throw;
        }
    }

    void MediaLibrary::DeleteGroup(motion::GroupMetadata const& group)
    {
        std::scoped_lock lock(mutex_);
        if (!motion::valid_id(group.id)) throw std::runtime_error("invalid group id");
        DeletePath(WallpapersPath() / L"Groups" / motion::utf8_to_wide(group.id));
    }

    std::string MediaLibrary::Import(fs::path const& source, std::string const& kind, std::string const& groupId,
        ImportProgress const& progress, std::atomic_bool const* cancelled)
    {
        auto extension = source.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(), towlower);
        static std::array<std::wstring, 6> const videos{ L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".avi" };
        static std::array<std::wstring, 8> const images{ L".jpg", L".jpeg", L".png", L".bmp", L".gif", L".tif", L".tiff", L".webp" };
        bool supported = kind == "image" ? std::find(images.begin(), images.end(), extension) != images.end()
            : kind == "video" && std::find(videos.begin(), videos.end(), extension) != videos.end();
        if (!supported || !motion::valid_id(groupId)) throw std::runtime_error("unsupported import");

        std::error_code sourceError;
        if (!fs::is_regular_file(source, sourceError) || sourceError || !fs::file_size(source, sourceError) || sourceError) {
            throw std::runtime_error("import source is not a readable file");
        }
        if (kind == "image") validate_image(source);
        else validate_video(source);

        auto groupDirectory = WallpapersPath() / L"Groups" / motion::utf8_to_wide(groupId);
        if (!fs::is_regular_file(groupDirectory / L"group.json")) throw std::runtime_error("import group does not exist");
        require_import_space(groupDirectory, source);
        auto mediaId = motion::new_id();
        auto directory = groupDirectory / L"Videos" / motion::utf8_to_wide(mediaId);
        fs::create_directories(directory);
        try {
            auto fileName = L"source" + extension;
            auto destination = directory / fileName;
            auto sourceHash = copy_and_sha256(source, destination, progress, cancelled);
            std::scoped_lock lock(mutex_);
            if (!fs::is_regular_file(groupDirectory / L"group.json")) throw std::runtime_error("import group was removed");
            auto siblings = directory.parent_path();
            for (auto const& entry : fs::directory_iterator(siblings)) {
                if (!entry.is_directory() || entry.path() == directory) continue;
                try {
                    auto existing = motion::load_media(entry.path() / L"metadata.json");
                    if (existing && existing->sha256 == sourceHash && existing->kind == kind) {
                        std::error_code ignored;
                        fs::remove_all(directory, ignored);
                        return existing->id;
                    }
                } catch (...) {}
            }
            motion::MediaMetadata media;
            media.id = mediaId;
            media.groupId = groupId;
            media.name = source.stem().wstring();
            media.kind = kind;
            media.originalName = source.filename().wstring();
            media.fileName = fileName;
            media.sha256 = sourceHash;
            media.sizeBytes = fs::file_size(destination);
            media.revision = 1;
            media.importedAt = media.updatedAt = motion::timestamp_utc();
            if (kind == "image") media.coverFileName = fileName;
            motion::save_media(directory / L"metadata.json", media);
            return mediaId;
        } catch (...) {
            std::error_code ignored;
            fs::remove_all(directory, ignored);
            throw;
        }
    }

    fs::path MediaLibrary::MediaDirectory(motion::MediaMetadata const& media) const
    {
        if (!motion::valid_id(media.groupId) || !motion::valid_id(media.id)) throw std::runtime_error("invalid media id");
        return WallpapersPath() / L"Groups" / motion::utf8_to_wide(media.groupId) / L"Videos" / motion::utf8_to_wide(media.id);
    }

    fs::path MediaLibrary::ResolveMediaDirectory(motion::MediaMetadata const& media) const
    {
        auto preferred = MediaDirectory(media);
        try {
            if (auto current = motion::load_media(preferred / L"metadata.json");
                current && current->id == media.id) return preferred;
        } catch (...) {}
        std::error_code error;
        auto groups = WallpapersPath() / L"Groups";
        for (fs::directory_iterator entries(groups, error), end; !error && entries != end; entries.increment(error)) {
            auto candidate = entries->path() / L"Videos" / motion::utf8_to_wide(media.id);
            try {
                auto current = motion::load_media(candidate / L"metadata.json");
                if (current && current->id == media.id) return candidate;
            } catch (...) {}
        }
        throw std::runtime_error("media no longer exists");
    }

    void MediaLibrary::Rename(motion::MediaMetadata const& media, std::wstring const& value)
    {
        std::scoped_lock lock(mutex_);
        auto name = trim(value);
        if (name.empty()) throw std::runtime_error("empty media name");
        auto path = ResolveMediaDirectory(media) / L"metadata.json";
        auto loaded = motion::load_media(path);
        if (!loaded || loaded->id != media.id) throw std::runtime_error("media changed during rename");
        auto updated = std::move(*loaded);
        updated.name = std::move(name);
        ++updated.revision;
        updated.updatedAt = motion::timestamp_utc();
        motion::save_media(path, updated);
    }

    void MediaLibrary::UpdateCover(motion::MediaMetadata const& media, std::wstring const& coverFileName)
    {
        std::scoped_lock lock(mutex_);
        if (!motion::safe_file_name(coverFileName)) throw std::runtime_error("invalid cover file name");
        auto directory = ResolveMediaDirectory(media);
        if (!fs::is_regular_file(directory / coverFileName)) throw std::runtime_error("cover does not exist");
        auto loaded = motion::load_media(directory / L"metadata.json");
        if (!loaded || loaded->id != media.id) throw std::runtime_error("media changed during cover update");
        auto updated = std::move(*loaded);
        updated.coverFileName = coverFileName;
        ++updated.revision;
        updated.updatedAt = motion::timestamp_utc();
        motion::save_media(directory / L"metadata.json", updated);
    }

    bool MediaLibrary::EnsureCover(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        HRESULT apartmentResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(apartmentResult) && apartmentResult != RPC_E_CHANGED_MODE) return false;
        struct ApartmentRelease
        {
            bool initialized{};
            ~ApartmentRelease() { if (initialized) CoUninitialize(); }
        } apartment{ SUCCEEDED(apartmentResult) };
        auto directory = ResolveMediaDirectory(media);
        auto current = motion::load_media(directory / L"metadata.json");
        if (!current || current->id != media.id) return false;
        if (!current->coverFileName.empty()) {
            std::error_code coverError;
            if (fs::is_regular_file(directory / current->coverFileName, coverError) && !coverError) return false;
        }
        auto poster = directory / L"poster.png";
        bool generated = current->kind == "image"
            ? ThumbnailGenerator::EnsureImageCover(directory / current->fileName, poster)
            : ThumbnailGenerator::EnsureVideoCover(directory / current->fileName, poster);
        if (!generated) return false;
        if (current->coverFileName != L"poster.png") {
            current->coverFileName = L"poster.png";
            ++current->revision;
            current->updatedAt = motion::timestamp_utc();
            motion::save_media(directory / L"metadata.json", *current);
        }
        return true;
    }

    bool MediaLibrary::RequestOptimization(motion::MediaMetadata const& media, std::string const& mode)
    {
        std::scoped_lock lock(mutex_);
        if (media.kind != "video" || mode == "original") return false;
        auto directory = ResolveMediaDirectory(media);
        std::error_code error;
        if (!fs::is_regular_file(directory / media.fileName, error) || error) return false;
        return motion::request_variant_generation(directory, mode);
    }

    void MediaLibrary::PauseOptimization(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        if (!motion::pause_variant_generation(ResolveMediaDirectory(media))) {
            throw std::runtime_error("unable to persist optimization pause");
        }
    }

    void MediaLibrary::ResumeOptimization(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        if (!motion::resume_variant_generation(ResolveMediaDirectory(media))) {
            throw std::runtime_error("unable to resume optimization");
        }
    }

    void MediaLibrary::CancelOptimization(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        if (!motion::cancel_variant_generation(ResolveMediaDirectory(media))) {
            throw std::runtime_error("unable to persist optimization cancellation");
        }
    }

    void MediaLibrary::SuppressOptimization(motion::MediaMetadata const& media, std::string const& mode)
    {
        std::scoped_lock lock(mutex_);
        if (!motion::suppress_variant_generation(ResolveMediaDirectory(media), mode)) {
            throw std::runtime_error("unable to suppress optimization profile");
        }
    }

    void MediaLibrary::DeleteVariantProfile(motion::MediaMetadata const& media, std::string const& mode)
    {
        std::scoped_lock lock(mutex_);
        if (mode != "balanced" && mode != "power-saver") {
            throw std::invalid_argument("unknown optimization profile");
        }
        auto directory = ResolveMediaDirectory(media);
        auto status = motion::inspect_variant_cache(directory);
        std::error_code sourceError;
        bool sourceAvailable = fs::is_regular_file(directory / media.fileName, sourceError) && !sourceError;
        if (!sourceAvailable) {
            bool hasRetainedProfile = std::any_of(status.entries.begin(), status.entries.end(),
                [&](auto const& entry) { return entry.mode != mode && entry.bytes; });
            if (!hasRetainedProfile) throw std::runtime_error("cannot delete the last playable media file");
        }
        auto variants = directory / L"Variants";
        auto prefix = motion::utf8_to_wide(mode) + L"-";
        std::error_code error;
        if (!fs::exists(variants, error)) {
            if (error) throw std::system_error(error);
            return;
        }
        for (fs::directory_iterator entry(variants, error), end; !error && entry != end; entry.increment(error)) {
            std::error_code itemError;
            if (!entry->is_regular_file(itemError) || itemError) continue;
            auto name = entry->path().filename().wstring();
            if (!name.starts_with(prefix)) continue;
            fs::remove(entry->path(), itemError);
            if (itemError) throw std::system_error(itemError);
        }
        if (error) throw std::system_error(error);
        fs::remove(variants, error);
        if (error && error != std::errc::directory_not_empty) throw std::system_error(error);
    }

    void MediaLibrary::DeleteVariants(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        auto directory = ResolveMediaDirectory(media);
        std::error_code sourceError;
        if (!fs::is_regular_file(directory / media.fileName, sourceError) || sourceError) {
            throw std::runtime_error("cannot delete the last playable media files");
        }
        if (!motion::cancel_variant_generation(directory)) {
            throw std::runtime_error("unable to persist optimization cancellation");
        }
        std::error_code error;
        fs::remove_all(directory / L"Variants", error);
        if (error || fs::exists(directory / L"Variants")) {
            throw std::system_error(error ? error : std::make_error_code(std::errc::operation_canceled));
        }
    }

    motion::VariantCacheStatus MediaLibrary::VariantStatus(motion::MediaMetadata const& media) const
    {
        std::scoped_lock lock(mutex_);
        try { return motion::inspect_variant_cache(ResolveMediaDirectory(media)); }
        catch (...) { return {}; }
    }

    bool MediaLibrary::SourceAvailable(motion::MediaMetadata const& media) const
    {
        std::scoped_lock lock(mutex_);
        try {
            auto directory = ResolveMediaDirectory(media);
            std::error_code error;
            return fs::is_regular_file(directory / media.fileName, error) && !error;
        } catch (...) {
            return false;
        }
    }

    void MediaLibrary::DeleteSource(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        auto directory = ResolveMediaDirectory(media);
        auto current = motion::load_media(directory / L"metadata.json");
        if (!current || current->id != media.id || current->kind != "video") {
            throw std::runtime_error("only video sources can be removed");
        }
        auto source = directory / current->fileName;
        std::error_code sourceError;
        if (!fs::is_regular_file(source, sourceError) || sourceError) return;

        auto status = motion::inspect_variant_cache(directory);
        bool hasPlayableCopy = std::any_of(status.entries.begin(), status.entries.end(),
            [](auto const& entry) { return entry.bytes != 0; });
        if (!hasPlayableCopy) throw std::runtime_error("no playable performance copy exists");

        std::error_code coverError;
        bool hasCover = !current->coverFileName.empty() &&
            fs::is_regular_file(directory / current->coverFileName, coverError) && !coverError;
        if (!hasCover) {
            auto poster = directory / L"poster.png";
            if (!ThumbnailGenerator::EnsureVideoCover(source, poster)) {
                throw std::runtime_error("unable to preserve the wallpaper poster");
            }
            current->coverFileName = L"poster.png";
            ++current->revision;
            current->updatedAt = motion::timestamp_utc();
            motion::save_media(directory / L"metadata.json", *current);
        }

        if (!motion::cancel_variant_generation(directory)) {
            throw std::runtime_error("unable to stop source-dependent optimization");
        }
        DeletePath(source);
    }

    void MediaLibrary::Move(motion::MediaMetadata const& media, std::string const& targetGroupId)
    {
        std::scoped_lock lock(mutex_);
        if (!motion::valid_id(targetGroupId)) return;
        auto source = ResolveMediaDirectory(media);
        auto sourceMetadata = motion::load_media(source / L"metadata.json");
        if (!sourceMetadata || sourceMetadata->id != media.id) throw std::runtime_error("media changed during move");
        if (sourceMetadata->groupId == targetGroupId) return;
        auto targetGroup = WallpapersPath() / L"Groups" / motion::utf8_to_wide(targetGroupId);
        if (!fs::is_regular_file(targetGroup / L"group.json")) throw std::runtime_error("move target group does not exist");
        auto targetRoot = targetGroup / L"Videos";
        auto target = targetRoot / motion::utf8_to_wide(media.id);
        if (fs::exists(target)) throw std::runtime_error("duplicate move target");
        fs::create_directories(targetRoot);
        auto staging = targetRoot / (L".moving-" + motion::utf8_to_wide(media.id.substr(0, 12)));
        std::error_code cleanupError;
        fs::remove_all(staging, cleanupError);
        DWORD moveError = ERROR_SUCCESS;
        for (int attempt = 0; attempt < 30; ++attempt) {
            if (MoveFileExW(source.c_str(), staging.c_str(), MOVEFILE_WRITE_THROUGH)) { moveError = ERROR_SUCCESS; break; }
            moveError = GetLastError();
            Sleep(100);
        }
        if (moveError != ERROR_SUCCESS) throw std::system_error(static_cast<int>(moveError), std::system_category());
        try {
            auto loaded = motion::load_media(staging / L"metadata.json");
            if (!loaded || loaded->id != media.id || loaded->groupId != sourceMetadata->groupId) {
                throw std::runtime_error("media changed during move");
            }
            loaded->groupId = targetGroupId;
            ++loaded->revision;
            loaded->updatedAt = motion::timestamp_utc();
            motion::save_media(staging / L"metadata.json", *loaded);
            if (!MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
            }
        } catch (...) {
            try {
                if (auto rollback = motion::load_media(staging / L"metadata.json")) {
                    rollback->groupId = sourceMetadata->groupId;
                    ++rollback->revision;
                    rollback->updatedAt = motion::timestamp_utc();
                    motion::save_media(staging / L"metadata.json", *rollback);
                }
            } catch (...) {}
            MoveFileExW(staging.c_str(), source.c_str(), MOVEFILE_WRITE_THROUGH);
            throw;
        }
    }

    void MediaLibrary::Delete(motion::MediaMetadata const& media)
    {
        std::scoped_lock lock(mutex_);
        DeletePath(ResolveMediaDirectory(media));
    }

    void MediaLibrary::DeletePath(fs::path const& path) const
    {
        if (!fs::exists(path)) return;
        if (deleteMode_ == DeleteMode::Permanent) {
            std::error_code error;
            fs::remove_all(path, error);
            if (error || fs::exists(path)) throw std::system_error(error ? error : std::make_error_code(std::errc::operation_canceled));
            return;
        }
        std::wstring source = path.wstring();
        source.push_back(L'\0');
        source.push_back(L'\0');
        SHFILEOPSTRUCTW operation{};
        operation.wFunc = FO_DELETE;
        operation.pFrom = source.c_str();
        operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        int result = SHFileOperationW(&operation);
        if (result || operation.fAnyOperationsAborted || fs::exists(path)) {
            throw std::system_error(result ? result : ERROR_CANCELLED, std::system_category());
        }
    }

}
