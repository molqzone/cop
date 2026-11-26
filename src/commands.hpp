#pragma once

#include <cstdint>
#include <string>

namespace cop
{

/// @brief 解析后的命令行选项 / Parsed command-line options.
struct Options
{
  std::string uio_name = "guidance-pages";
  std::string page;  ///< `--page FILE`: host testing instead of a UIO device.
  std::string verb;
  uint32_t offset = 0;
  uint32_t bytes = 256;
  bool new_only = false;
  uint16_t type = 0;
  bool type_set = false;
  std::string payload;
  uint32_t timeout_s = 30;
  bool confirm = false;
};

/// @brief 执行一个 verb；返回进程退出码 / Run one verb; returns the process exit code.
int Run(const Options& options);

int List(const Options& options);
int Status(const Options& options);
int Dump(const Options& options);
int Log(const Options& options);
int Watch(const Options& options);
int Send(const Options& options);
int WaitConsole(const Options& options);
int WipeShared(const Options& options);

}  // namespace cop
