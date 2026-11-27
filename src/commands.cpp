#include "commands.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>

#include "console_page.hpp"
#include "linux_cmdqu.hpp"
#include "page_mapping.hpp"
#include "page_view.hpp"

namespace cop
{
namespace
{

int Fail(const std::string& message)
{
  std::fprintf(stderr, "cop: %s\n", message.c_str());
  return 1;
}

size_t FileBytes(const std::string& path, size_t fallback)
{
  struct stat info = {};
  if (::stat(path.c_str(), &info) == 0 && info.st_size > 0)
  {
    return static_cast<size_t>(info.st_size);
  }
  return fallback;
}

Mapping OpenShared(const Options& options)
{
  if (!options.page.empty())
  {
    return Mapping::OpenFile(options.page, FileBytes(options.page, 4096));
  }
  return Mapping::OpenUio(options.uio_name, "shared", 1);
}

Mapping OpenConsole(const Options& options)
{
  if (!options.page.empty())
  {
    return Mapping::OpenFile(options.page, FileBytes(options.page, 4096));
  }
  return Mapping::OpenUio(options.uio_name, "console", 0);
}

uint32_t U32At(const uint8_t* base, size_t offset)
{
  const auto* word = reinterpret_cast<const volatile uint32_t*>(base + offset);
  return *word;
}

int HexNibble(char value)
{
  if (value >= '0' && value <= '9')
  {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f')
  {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F')
  {
    return value - 'A' + 10;
  }
  return -1;
}

bool ParseHex(const std::string& text, std::vector<uint8_t>* out)
{
  if (out == nullptr || text.size() % 2u != 0u)
  {
    return false;
  }
  out->clear();
  out->reserve(text.size() / 2u);
  for (size_t index = 0; index < text.size(); index += 2u)
  {
    const int high = HexNibble(text[index]);
    const int low = HexNibble(text[index + 1u]);
    if (high < 0 || low < 0)
    {
      return false;
    }
    out->push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return true;
}

void PrintSlot(const uint8_t* slot, size_t bytes)
{
  for (size_t index = 0; index < bytes; ++index)
  {
    std::printf("%02x", slot[index]);
  }
}

}  // namespace

int Status(const Options& options)
{
  Mapping mapping = OpenShared(options);
  if (!mapping.Valid())
  {
    return Fail("cannot map the shared page (" + mapping.Origin() + ")");
  }

  LibXR::PageView<LibXR::NoCoherence> view(mapping.Data(), mapping.Size());
  const LibXR::PageMagicStatus status = view.Check();
  const LibXR::PageHeader header = view.Header();

  std::printf("mapping   : %s\n", mapping.Origin().c_str());
  std::printf("page      : %s magic=0x%08x\n", LibXR::MagicName(status), header.magic);
  if (status != LibXR::PageMagicStatus::FORMATTED)
  {
    return status == LibXR::PageMagicStatus::MISMATCH ? 1 : 0;
  }

  const auto& layout = view.Layout();
  std::printf("geometry  : size=%u slots=%u slot=%u payload=%u region=%u tag=%u\n",
              layout.page_size, layout.slot_count, layout.sample_size,
              layout.payload_size, layout.region_offset, layout.tag);
  std::printf("layout    : ring=%u head=%u write_state=%u channels=%u\n",
              layout.telemetry_offset, layout.head_offset, layout.write_state_offset,
              layout.channels_offset);

  auto channels = view.Channels();
  const uint32_t count = channels.Valid() ? channels.Count() : 0u;
  const uint32_t generation = channels.Valid() ? channels.Generation() : 0u;
  std::printf("channels  : %u announced (gen %u) at %u\n", count, generation,
              layout.channels_offset);
  for (uint32_t index = 0; index < count; ++index)
  {
    LibXR::ChannelEntry entry{};
    if (!channels.Entry(index, &entry))
    {
      continue;
    }
    std::printf("            %-12.*s endpoint=0x%x flags=%u key=0x%08x\n",
                static_cast<int>(entry.name_len), entry.name, entry.endpoint,
                entry.flags, entry.key);
  }

  std::printf("head      : %u (write_state %u)\n", view.Head(),
              U32At(mapping.Data(), layout.write_state_offset));
  std::printf("region    : seq=%u (offset %u)\n", view.Seq(), layout.region_offset);
  return 0;
}

int Dump(const Options& options)
{
  Mapping mapping = OpenShared(options);
  if (!mapping.Valid())
  {
    return Fail("cannot map the shared page (" + mapping.Origin() + ")");
  }
  if (options.offset >= mapping.Size())
  {
    return Fail("offset is past the end of the page");
  }

  const size_t bytes = std::min<size_t>(options.bytes, mapping.Size() - options.offset);
  for (size_t line = 0; line < bytes; line += 16u)
  {
    const size_t at = options.offset + line;
    const size_t chunk = std::min<size_t>(16u, bytes - line);
    std::printf("%04zx  ", at);
    for (size_t index = 0; index < 16u; ++index)
    {
      if (index < chunk)
      {
        std::printf("%02x ", mapping.Data()[at + index]);
      }
      else
      {
        std::printf("   ");
      }
    }
    std::printf(" ");
    for (size_t index = 0; index < chunk; ++index)
    {
      const uint8_t value = mapping.Data()[at + index];
      std::printf("%c", value >= 32u && value < 127u ? value : '.');
    }
    std::printf("\n");
  }
  return 0;
}

int List(const Options& options)
{
  const auto devices = LibXR::UIO::Enumerate("/sys/class/uio");
  const auto found = std::find_if(
      devices.begin(), devices.end(), [&](const LibXR::UIO::DeviceInfo& device) {
        return device.name == options.uio_name;
      });
  if (found == devices.end())
  {
    return Fail(options.uio_name + " uio device not found");
  }

  std::printf("%s (%s)\n", found->name.c_str(), found->dir.c_str());
  for (const auto& map : found->maps)
  {
    std::printf("  map %u  %-8s addr=0x%08llx size=%zu\n", map.index, map.name.c_str(),
                static_cast<unsigned long long>(map.addr), map.bytes);
  }
  return 0;
}

int Log(const Options& options)
{
  Mapping mapping = OpenConsole(options);
  if (!mapping.Valid())
  {
    return Fail("cannot map the console page (" + mapping.Origin() + ")");
  }

  LibXR::ConsoleView view(mapping.Data(), mapping.Size());
  if (!view.Valid())
  {
    return Fail("console page not published (descriptor magic/version/bounds)");
  }

  const uint32_t ring = view.RingBytes();
  uint32_t total = view.Index();
  uint32_t seen = options.new_only ? total : (total < ring ? 0u : total - ring);

  for (;;)
  {
    total = view.Index();
    if (total < seen)
    {
      seen = total < ring ? 0u : total - ring;
    }
    if (total - seen > ring)
    {
      std::fprintf(stderr, "cop: console ring overrun, %u bytes lost\n",
                   total - seen - ring);
      seen = total - ring;
    }

    while (seen < total)
    {
      const uint32_t start = seen % ring;
      const size_t chunk = std::min<size_t>(
          {static_cast<size_t>(total - seen), static_cast<size_t>(ring - start), 256u});
      std::array<uint8_t, 256> buffer{};
      if (!view.Read(start, buffer.data(), chunk))
      {
        break;
      }
      std::fwrite(buffer.data(), 1, chunk, stdout);
      std::fflush(stdout);
      seen += static_cast<uint32_t>(chunk);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

int Watch(const Options& options)
{
  Mapping mapping = OpenShared(options);
  if (!mapping.Valid())
  {
    return Fail("cannot map the shared page (" + mapping.Origin() + ")");
  }

  LibXR::PageView<LibXR::NoCoherence> view(mapping.Data(), mapping.Size());
  if (view.Check() != LibXR::PageMagicStatus::FORMATTED)
  {
    return Fail(std::string("page is ") + LibXR::MagicName(view.Check()) +
                "; a watcher needs a page its owner has built");
  }

  const auto& layout = view.Layout();
  const uint32_t capacity = std::min<uint32_t>(layout.slot_count, 64u);
  std::vector<uint8_t> slots(static_cast<size_t>(capacity) * layout.sample_size);
  uint32_t last = view.Head();

  for (;;)
  {
    LibXR::SinceResult result{};
    const LibXR::ErrorCode code =
        view.Since(last, slots.data(), capacity, &result);
    if (code == LibXR::ErrorCode::OK)
    {
      for (uint32_t index = 0; index < result.written; ++index)
      {
        const uint32_t sequence = result.next - result.written + index;
        std::printf("%u  ", sequence);
        PrintSlot(slots.data() + static_cast<size_t>(index) * layout.sample_size,
                  layout.sample_size);
        std::printf("\n");
      }
      std::fflush(stdout);
      last = result.next;
    }
    else if (code == LibXR::ErrorCode::EMPTY)
    {
      if (result.dropped != 0u)
      {
        std::fprintf(stderr, "cop: %u sample(s) dropped\n", result.dropped);
      }
      last = result.next;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

int Send(const Options& options)
{
  if (!options.type_set)
  {
    return Fail("send needs --type");
  }

  Mapping mapping = OpenShared(options);
  if (!mapping.Valid())
  {
    return Fail("cannot map the shared page (" + mapping.Origin() + ")");
  }

  LibXR::PageView<LibXR::NoCoherence> view(mapping.Data(), mapping.Size());
  if (view.Check() != LibXR::PageMagicStatus::FORMATTED)
  {
    return Fail(std::string("page is ") + LibXR::MagicName(view.Check()) +
                "; a command needs a page its owner has built");
  }

  const auto& layout = view.Layout();
  if (layout.command_magic == 0u)
  {
    return Fail("this page has no command slot");
  }

  std::vector<uint8_t> payload;
  if (!ParseHex(options.payload, &payload))
  {
    return Fail("--payload is not an even-length hex string");
  }
  const size_t limit = layout.payload_size - sizeof(LibXR::CommandHeader);
  if (payload.size() > limit)
  {
    return Fail("payload is " + std::to_string(payload.size()) + " bytes, the page leaves " +
                std::to_string(limit));
  }

  LibXR::CmdquMailbox mailbox;
  if (!mailbox.Ready())
  {
    return Fail("cannot open /dev/cvi-rtos-cmdqu (is the cmdqu kernel module loaded?)");
  }

  uint32_t seq = 0;
  LibXR::ErrorCode code =
      view.WriteCommand(options.type, payload.data(), payload.size(), &seq);
  if (code != LibXR::ErrorCode::OK)
  {
    return Fail(std::string("cannot write the command (") +
                std::to_string(static_cast<int>(code)) + ")");
  }

  uint32_t reply = 0;
  code = mailbox.SendWait(seq, 3000u, &reply);
  if (code != LibXR::ErrorCode::OK)
  {
    return Fail(std::string("mailbox send/wait failed (") +
                std::to_string(static_cast<int>(code)) + ")");
  }

  std::printf("sent type=%u seq=%u, reply=0x%08x\n", options.type, seq, reply);
  // The wire command header carries a 16-bit sequence; the kernel reply is a
  // 32-bit word holding that same 16-bit value.  Compare the truncated forms,
  // otherwise every send after seq 65535 looks like a failure.
  return static_cast<uint16_t>(reply) == static_cast<uint16_t>(seq) ? 0 : 1;
}

int WaitConsole(const Options& options)
{
  Mapping mapping = OpenConsole(options);
  if (!mapping.Valid())
  {
    return Fail("cannot map the console page (" + mapping.Origin() + ")");
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_s);
  for (;;)
  {
    LibXR::ConsoleView view(mapping.Data(), mapping.Size());
    if (view.Valid())
    {
      return 0;
    }
    if (std::chrono::steady_clock::now() >= deadline)
    {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return Fail("console page did not appear within " +
              std::to_string(options.timeout_s) + "s");
}

int WipeShared(const Options& options)
{
  if (!options.confirm)
  {
    return Fail("wipe-shared needs --confirm (boot path only)");
  }

  Mapping mapping = OpenShared(options);
  if (!mapping.Valid())
  {
    return Fail("cannot map the shared page (" + mapping.Origin() + ")");
  }

  std::memset(mapping.Data(), 0, mapping.Size());
  __sync_synchronize();
  std::printf("wiped %zu bytes at %s\n", mapping.Size(), mapping.Origin().c_str());
  return 0;
}

int Run(const Options& options)
{
  if (options.verb == "list")
  {
    return List(options);
  }
  if (options.verb == "status")
  {
    return Status(options);
  }
  if (options.verb == "dump")
  {
    return Dump(options);
  }
  if (options.verb == "log")
  {
    return Log(options);
  }
  if (options.verb == "watch")
  {
    return Watch(options);
  }
  if (options.verb == "send")
  {
    return Send(options);
  }
  if (options.verb == "wait-console")
  {
    return WaitConsole(options);
  }
  if (options.verb == "wipe-shared")
  {
    return WipeShared(options);
  }

  std::fprintf(stderr, "cop: unknown verb %s\n", options.verb.c_str());
  return 2;
}

}  // namespace cop
