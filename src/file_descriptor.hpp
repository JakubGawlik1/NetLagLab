#pragma once

#include <unistd.h>

namespace netlaglab {

class FileDescriptor {
public:
    explicit FileDescriptor(const int descriptor) noexcept
        : descriptor_{descriptor}
    {
    }

    ~FileDescriptor()
    {
        reset();
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : descriptor_{other.release()}
    {
    }

    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other) {
            reset();
            descriptor_ = other.release();
        }

        return *this;
    }

    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

    void reset() noexcept
    {
        if (descriptor_ != -1) {
            close(descriptor_);
            descriptor_ = -1;
        }
    }

    [[nodiscard]] int release() noexcept
    {
        const int released_descriptor{descriptor_};
        descriptor_ = -1;
        return released_descriptor;
    }

private:
    int descriptor_;
};

} // namespace netlaglab
