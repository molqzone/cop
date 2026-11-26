/**
 * @file fixture.cpp
 * @brief 为 CLI 测试写一页固件会留下的字节 / Write the bytes a firmware leaves behind.
 *
 * 这不是产品契约：测试自有 2 KiB/32 槽几何，证明 `cop` 只读页头，不依赖任何产品
 * 常量。为了不把 LibXR 的 `Topic` 运行时拖进来，这里直接用 `PageCore` /
 * `TelemetryCore` / `ReferenceCore` / `NameService` 写同一页字节。
 * This is not a product contract: the test declares its own 2 KiB/32-slot geometry,
 * proving `cop` reads the header only and depends on no product constant.  To avoid
 * pulling LibXR's `Topic` runtime in, the fixture builds the bytes through
 * `PageCore` / `TelemetryCore` / `ReferenceCore` / `NameService` directly.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>

#include "shared_page.hpp"

namespace
{
struct Format
{
  static constexpr size_t PAGE_SIZE = 2048;
  static constexpr uint32_t SLOT_COUNT = 32;
  static constexpr uint32_t COMMAND_MAGIC = 0x31444D47U;
};

struct Sample
{
  uint32_t values[8];
};

struct CommandPayload
{
  uint32_t magic;
  uint16_t type;
  uint16_t seq;
  uint8_t bytes[16];
};

constexpr uint32_t TAG = 0x54414731U;
using Layout = decltype(LibXR::make_page_layout<Format, Sample, CommandPayload, TAG>());
constexpr Layout LAYOUT =
    LibXR::make_page_layout<Format, Sample, CommandPayload, TAG>();
}  // namespace

int main(int argc, char** argv)
{
  if (argc != 2)
  {
    return 2;
  }

  std::array<uint8_t, Format::PAGE_SIZE> storage{};
  LibXR::PageCore<LibXR::NoCoherence> core(storage.data(), LAYOUT,
                                           LibXR::NoCoherence{});
  core.Format();

  Sample sample{};
  for (uint32_t index = 0; index < 8; ++index)
  {
    sample.values[index] = 0x100u + index;
  }
  LibXR::TelemetryCore<LibXR::NoCoherence> telemetry(storage.data(), LAYOUT,
                                                      LibXR::NoCoherence{});
  telemetry.Write(&sample);

  LibXR::NameService<4, LibXR::NoCoherence> channels(storage.data(), LAYOUT,
                                                     LibXR::NoCoherence{});
  const LibXR::ChannelAnnouncement announcements[] = {
      {"imu", 0x400u, LibXR::CHANNEL_FLAG_PRESENT},
  };
  channels.Publish(announcements, 1u);

  CommandPayload command{};
  command.magic = Format::COMMAND_MAGIC;
  command.type = 4u;
  command.seq = 1u;
  for (uint8_t index = 0; index < sizeof(command.bytes); ++index)
  {
    command.bytes[index] = static_cast<uint8_t>(0x40u + index);
  }
  LibXR::ReferenceCore<LibXR::NoCoherence> reference(storage.data(), LAYOUT,
                                                     LibXR::NoCoherence{});
  reference.Write(&command);

  std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
  if (!out.good())
  {
    return 1;
  }
  out.write(reinterpret_cast<const char*>(storage.data()),
            static_cast<std::streamsize>(storage.size()));
  return out.good() ? 0 : 1;
}
