#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <absl/status/status.h>

namespace chronolog
{

class FileSink
{
public:
    virtual ~FileSink() = default;
    virtual absl::Status write(std::string_view bytes) = 0;
    virtual absl::Status sync() = 0;
};

std::unique_ptr<FileSink> openFileSink(const std::string& path);

} // namespace chronolog
