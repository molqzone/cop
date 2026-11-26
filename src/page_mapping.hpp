#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "linux_mapping.hpp"

namespace cop
{

/**
 * @brief 一段页映射：UIO 设备，或 `--page` 给的普通文件 / One page mapping.
 *
 * UIO 路径走 `LibXR::UIOMapping`（按 DT 的 `reg-names` 找 map，不写死索引）；文件
 * 路径只用于主机测试，`cop --page fixture status` 不需要板子。
 * The UIO path goes through `LibXR::UIOMapping` (map found by the DT `reg-names`, no
 * hardcoded index); the file path is for host tests only, so `cop --page fixture status`
 * needs no board.
 */
class Mapping
{
 public:
  Mapping() = default;

  static Mapping OpenUio(const std::string& uio_name, const std::string& map_name,
                         uint32_t fallback_index)
  {
    Mapping mapping;
    mapping.uio_ = LibXR::UIOMapping::OpenNamed(
        uio_name.c_str(), map_name.c_str(), fallback_index, 4096);
    mapping.data_ = mapping.uio_.Data();
    mapping.size_ = mapping.uio_.Size();
    mapping.origin_ = uio_name + ":" + map_name;
    return mapping;
  }

  static Mapping OpenFile(const std::string& path, size_t bytes)
  {
    Mapping mapping;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0)
    {
      return mapping;
    }
    if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0)
    {
      ::close(fd);
      return mapping;
    }
    void* base = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (base == MAP_FAILED)
    {
      return mapping;
    }
    mapping.data_ = static_cast<uint8_t*>(base);
    mapping.size_ = bytes;
    mapping.origin_ = path;
    return mapping;
  }

  [[nodiscard]] uint8_t* Data() const { return data_; }
  [[nodiscard]] size_t Size() const { return size_; }
  [[nodiscard]] const std::string& Origin() const { return origin_; }
  [[nodiscard]] bool Valid() const { return data_ != nullptr && size_ != 0; }

 private:
  LibXR::UIOMapping uio_;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  std::string origin_;
};

}  // namespace cop
