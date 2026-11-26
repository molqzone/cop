/**
 * @file main.cpp
 * @brief `cop` 的参数解析 / Argument parsing for `cop`.
 *
 * 这个工具只做传输层：页格式由写页的核决定，`cop` 在运行时从 `PageHeader` /
 * `ConsoleDescriptor` 读出来。没有 `--contract`，也没有产品头文件依赖。
 * This tool is transport only: the core that writes the page owns the format, and `cop`
 * discovers it at runtime from `PageHeader` / `ConsoleDescriptor`.  There is no
 * `--contract` and no product-header dependency.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "commands.hpp"

namespace
{

void Usage(std::FILE* out)
{
  std::fprintf(
      out,
      "cop - Linux-side CLI for a coprocessor shared page\n"
      "usage: cop [--uio-name NAME] [--page FILE] <verb> [options]\n"
      "\n"
      "verbs:\n"
      "  list                              pages the device tree exposes\n"
      "  status                            decode the self-describing page header\n"
      "  dump --offset N --bytes N         hex dump\n"
      "  log [--new]                       stream the console ring\n"
      "  watch                             print new telemetry slots as hex\n"
      "  send --type N [--payload HEX]     one generic command (magic/type/seq + payload)\n"
      "  wait-console [--timeout S]        wait for the console descriptor\n"
      "  wipe-shared --confirm             zero the shared page (boot path only)\n"
      "\n"
      "The page describes itself; there is no contract file to pass.\n");
}

bool ParseU32(const char* text, uint32_t* out)
{
  if (text == nullptr || out == nullptr || *text == '\0')
  {
    return false;
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(text, &end, 0);
  if (end == text || *end != '\0' || value > 0xFFFFFFFFUL)
  {
    return false;
  }
  *out = static_cast<uint32_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv)
{
  cop::Options options;

  for (int index = 1; index < argc; ++index)
  {
    const std::string argument = argv[index];
    const auto value = [&](const char* name) -> const char* {
      if (index + 1 >= argc)
      {
        std::fprintf(stderr, "cop: %s needs a value\n", name);
        std::exit(2);
      }
      return argv[++index];
    };

    if (argument == "--uio-name")
    {
      options.uio_name = value("--uio-name");
    }
    else if (argument == "--page")
    {
      options.page = value("--page");
    }
    else if (argument == "--offset")
    {
      if (!ParseU32(value("--offset"), &options.offset))
      {
        std::fprintf(stderr, "cop: --offset needs a number\n");
        return 2;
      }
    }
    else if (argument == "--bytes")
    {
      if (!ParseU32(value("--bytes"), &options.bytes))
      {
        std::fprintf(stderr, "cop: --bytes needs a number\n");
        return 2;
      }
    }
    else if (argument == "--type")
    {
      uint32_t type = 0;
      if (!ParseU32(value("--type"), &type) || type > 0xFFFFu)
      {
        std::fprintf(stderr, "cop: --type needs a 16-bit number\n");
        return 2;
      }
      options.type = static_cast<uint16_t>(type);
      options.type_set = true;
    }
    else if (argument == "--payload")
    {
      options.payload = value("--payload");
    }
    else if (argument == "--timeout")
    {
      if (!ParseU32(value("--timeout"), &options.timeout_s))
      {
        std::fprintf(stderr, "cop: --timeout needs seconds\n");
        return 2;
      }
    }
    else if (argument == "--new")
    {
      options.new_only = true;
    }
    else if (argument == "--confirm")
    {
      options.confirm = true;
    }
    else if (argument == "--help" || argument == "-h")
    {
      Usage(stdout);
      return 0;
    }
    else if (argument == "--contract")
    {
      std::fprintf(stderr,
                   "cop: --contract is gone; the page describes itself at runtime\n");
      return 2;
    }
    else if (!argument.empty() && argument[0] == '-')
    {
      std::fprintf(stderr, "cop: unknown option %s\n", argument.c_str());
      return 2;
    }
    else if (options.verb.empty())
    {
      options.verb = argument;
    }
    else
    {
      std::fprintf(stderr, "cop: unexpected argument %s\n", argument.c_str());
      return 2;
    }
  }

  if (options.verb.empty())
  {
    Usage(stderr);
    return 2;
  }
  return cop::Run(options);
}
