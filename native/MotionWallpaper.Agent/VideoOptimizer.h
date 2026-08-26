#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace motion::agent
{
    class VideoOptimizer
    {
    public:
        VideoOptimizer(
            std::filesystem::path dataRoot,
            std::filesystem::path applicationRoot);
        ~VideoOptimizer();
        VideoOptimizer(VideoOptimizer const&) = delete;
        VideoOptimizer& operator=(VideoOptimizer const&) = delete;

        [[nodiscard]] std::filesystem::path Resolve(
            std::filesystem::path const& source, std::string const& performanceMode,
            uint32_t targetWidth = 0, uint32_t targetHeight = 0,
            uint32_t targetRefreshRate = 0,
            bool softwarePlaybackTarget = false);
        void Prepare(std::filesystem::path const& source, std::string const& performanceMode,
            uint32_t targetWidth = 0, uint32_t targetHeight = 0,
            uint32_t targetRefreshRate = 0);
        void SetGenerationAllowed(bool allowed);
        void InvalidateChoices();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
