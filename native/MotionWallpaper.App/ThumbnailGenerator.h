#pragma once

#include <filesystem>

namespace motion::app
{
    class ThumbnailGenerator
    {
    public:
        static bool EnsureImageCover(std::filesystem::path const& source, std::filesystem::path const& destination);
        static bool GenerateImageCover(std::filesystem::path const& source, std::filesystem::path const& destination);
        static bool EnsureVideoCover(std::filesystem::path const& source, std::filesystem::path const& destination);
        static bool GenerateVideoCover(std::filesystem::path const& source, std::filesystem::path const& destination);
    };
}
