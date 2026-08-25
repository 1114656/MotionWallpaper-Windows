#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace motion::agent
{
    class VideoOptimizer
    {
    public:
        explicit VideoOptimizer(std::filesystem::path logRoot);
        ~VideoOptimizer();
        VideoOptimizer(VideoOptimizer const&) = delete;
        VideoOptimizer& operator=(VideoOptimizer const&) = delete;

        [[nodiscard]] std::filesystem::path Resolve(
            std::filesystem::path const& source, std::string const& performanceMode,
            uint32_t targetWidth = 0, uint32_t targetHeight = 0,
            uint32_t targetRefreshRate = 0);
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
