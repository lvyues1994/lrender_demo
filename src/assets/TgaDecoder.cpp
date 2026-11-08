#include "TgaDecoder.h"
#include <stdexcept>
namespace demo {
Result<lr::core::ImageData> decodeTga(std::vector<std::uint8_t> const &b, lr::core::ColorEncoding e) {
  return decodeTga(b, e, {});
}
Result<lr::core::ImageData> decodeTga(std::vector<std::uint8_t> const &b, lr::core::ColorEncoding encoding,
                                      std::function<bool()> const &stop) {
  try {
    auto check = [&] {
      if (stop && stop())
        throw std::runtime_error("cancelled");
    };
    auto need = [](bool condition, char const *message) {
      if (!condition)
        throw std::runtime_error(message);
    };
    check();
    need(b.size() >= 18 && b.size() <= 128u * 1024 * 1024, "TGA header/input size invalid or exceeds 128MiB");
    auto word = [&](unsigned p) { return unsigned(b[p]) | (unsigned(b[p + 1]) << 8); };
    auto type = b[2];
    bool gray = type == 3 || type == 11, rle = type == 10 || type == 11;
    need(type == 2 || type == 3 || type == 10 || type == 11, "unsupported TGA image type");
    need(b[1] == 0 && word(5) == 0 && b[7] == 0, "unsupported TGA color map");
    need((b[17] & 192) == 0, "unsupported interleaved TGA");
    auto bits = b[16];
    need(gray ? bits == 8 : (bits == 24 || bits == 32), "unsupported TGA pixel depth");
    need((b[17] & 15) == 0 || (!gray && bits == 32 && (b[17] & 15) == 8), "unsupported TGA attribute bits");
    auto width = word(12), height = word(14);
    std::uint64_t count = std::uint64_t(width) * height;
    need(width && height && count <= 64u * 1024 * 1024 / 4, "TGA decoded size exceeds 64MiB or is empty");
    std::size_t offset = 18u + b[0];
    need(offset <= b.size(), "truncated TGA image ID");
    unsigned channels = bits / 8;
    lr::core::ImageData image;
    image.extent = {int(width), int(height)};
    image.encoding = encoding;
    image.levels.emplace_back(std::size_t(count) * 4);
    auto &out = image.levels[0];
    std::size_t done = 0;
    auto pixel = [&](std::size_t source) {
      auto x = done % width, y = done / width;
      if (b[17] & 16)
        x = width - 1 - x;
      if (!(b[17] & 32))
        y = height - 1 - y;
      auto target = 4 * (y * width + x);
      out[target] = gray ? b[source] : b[source + 2];
      out[target + 1] = gray ? b[source] : b[source + 1];
      out[target + 2] = b[source];
      out[target + 3] = channels == 4 ? b[source + 3] : 255;
      ++done;
    };
    while (done < count) {
      check();
      unsigned run = 1;
      bool repeat = false;
      if (rle) {
        need(offset < b.size(), "truncated TGA packet header");
        auto packet = b[offset++];
        run = (packet & 127) + 1;
        repeat = packet & 128;
      }
      need(run <= count - done, "TGA packet overruns image");
      auto bytes = std::size_t(repeat ? 1 : run) * channels;
      need(bytes <= b.size() - offset, "truncated TGA pixel packet");
      for (unsigned i = 0; i < run; ++i)
        pixel(offset + (repeat ? 0 : i * channels));
      offset += bytes;
    }
    check();
    return image;
  } catch (std::exception const &e) {
    return RenderError{ErrorStage::Load, std::string(e.what()) == "cancelled", "TGA decode failed", e.what()};
  }
}
} // namespace demo
