#include "runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

extern "C" {
#include "../vendor/stb_image.h"
}

namespace dk3 {
namespace {
// Decoded and resized images are RGBA, 4 bytes a pixel; this caps one image at 256 MB.
constexpr uint64_t maxPixels = 64u * 1024 * 1024;

const std::string &bytesArgument(const std::vector<Value> &values, size_t index, const char *name, const Span &span) {
    if (index >= values.size() || !std::holds_alternative<Object *>(values[index]) || std::get<Object *>(values[index])->kind != Kind::Bytes)
        throw Error(span, std::string(name) + " must be Bytes", "type");
    return std::get<Object *>(values[index])->text;
}

int64_t intArgument(const std::vector<Value> &values, size_t index, const char *name, const Span &span) {
    if (index >= values.size() || !std::holds_alternative<int64_t>(values[index])) throw Error(span, std::string(name) + " must be an Int", "type");
    return std::get<int64_t>(values[index]);
}

void checkSize(int64_t width, int64_t height, const Span &span) {
    if (width < 1 || height < 1) throw Error(span, "Image dimensions must be positive", "value");
    // "value", not "limit": untrusted headers can claim any size and limit errors cannot be caught.
    if (static_cast<uint64_t>(width) > maxPixels || static_cast<uint64_t>(height) > maxPixels || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > maxPixels)
        throw Error(span, "Image is larger than " + std::to_string(maxPixels) + " pixels", "value");
}

// stb copies bytes of corrupt PNG chunk names into its reason, so keep only printable ASCII.
std::string failureReason() {
    const char *reason = stbi_failure_reason();
    std::string clean = reason ? reason : "unknown error";
    for (char &byte : clean)
        if (static_cast<unsigned char>(byte) < 0x20 || static_cast<unsigned char>(byte) > 0x7E) byte = '?';
    return clean;
}

struct Header { int width, height, channels; };

Header readHeader(const std::string &data, const Span &span) {
    Header header{};
    if (data.size() > static_cast<size_t>(INT32_MAX) ||
        !stbi_info_from_memory(reinterpret_cast<const stbi_uc *>(data.data()), static_cast<int>(data.size()), &header.width, &header.height, &header.channels))
        throw Error(span, "Unsupported or corrupt image: " + failureReason(), "value");
    checkSize(header.width, header.height, span);
    return header;
}
}

void Runtime::installImage() {
    registerNative("ImageCodec.info", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "info expects one Bytes value", "call");
        Header header = readHeader(bytesArgument(values, 0, "info data", span), span);
        Object *result = allocate(Kind::Tuple, span);
        result->items = {static_cast<int64_t>(header.width), static_cast<int64_t>(header.height), static_cast<int64_t>(header.channels)};
        return result;
    });
    registerNative("ImageCodec.decode", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 1) throw Error(span, "decode expects one Bytes value", "call");
        const std::string &data = bytesArgument(values, 0, "decode data", span);
        Header header = readHeader(data, span);
        int width = 0, height = 0, channels = 0;
        stbi_uc *pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(data.data()), static_cast<int>(data.size()), &width, &height, &channels, 4);
        if (!pixels) throw Error(span, "Cannot decode image: " + failureReason(), "value");
        std::string rgba(reinterpret_cast<const char *>(pixels), static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        stbi_image_free(pixels);
        if (width != header.width || height != header.height) throw Error(span, "Image header does not match its data", "value");
        return text(std::move(rgba), true, span);
    });
    // Area-average downscale (nearest when enlarging) of RGBA pixels; colour is weighted by alpha so transparent pixels do not tint it.
    registerNative("ImageCodec.resize", [this](const auto &arguments, const Span &span) -> Value {
        auto values = positional(arguments, span);
        if (values.size() != 5) throw Error(span, "resize expects pixels, width, height, newWidth, newHeight", "call");
        const std::string &source = bytesArgument(values, 0, "resize pixels", span);
        int64_t width = intArgument(values, 1, "width", span), height = intArgument(values, 2, "height", span);
        int64_t newWidth = intArgument(values, 3, "newWidth", span), newHeight = intArgument(values, 4, "newHeight", span);
        checkSize(width, height, span);
        checkSize(newWidth, newHeight, span);
        if (source.size() != static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4)
            throw Error(span, "pixels must hold width * height * 4 RGBA bytes", "value");
        std::string result(static_cast<size_t>(newWidth) * static_cast<size_t>(newHeight) * 4, '\0');
        const auto *in = reinterpret_cast<const unsigned char *>(source.data());
        auto *out = reinterpret_cast<unsigned char *>(result.data());
        for (int64_t y = 0; y < newHeight; y++) {
            tick(span);
            int64_t top = y * height / newHeight, bottom = std::max(top + 1, ((y + 1) * height + newHeight - 1) / newHeight);
            for (int64_t x = 0; x < newWidth; x++) {
                int64_t left = x * width / newWidth, right = std::max(left + 1, ((x + 1) * width + newWidth - 1) / newWidth);
                uint64_t red = 0, green = 0, blue = 0, alpha = 0, count = 0;
                for (int64_t sy = top; sy < std::min(bottom, height); sy++)
                    for (int64_t sx = left; sx < std::min(right, width); sx++) {
                        const unsigned char *pixel = in + (static_cast<size_t>(sy) * static_cast<size_t>(width) + static_cast<size_t>(sx)) * 4;
                        red += pixel[0] * pixel[3]; green += pixel[1] * pixel[3]; blue += pixel[2] * pixel[3]; alpha += pixel[3]; count++;
                    }
                unsigned char *target = out + (static_cast<size_t>(y) * static_cast<size_t>(newWidth) + static_cast<size_t>(x)) * 4;
                if (alpha) { target[0] = static_cast<unsigned char>(red / alpha); target[1] = static_cast<unsigned char>(green / alpha); target[2] = static_cast<unsigned char>(blue / alpha); }
                target[3] = static_cast<unsigned char>(count ? alpha / count : 0);
            }
        }
        return text(std::move(result), true, span);
    });
}
}
