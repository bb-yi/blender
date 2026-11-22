#pragma once

#include <memory>

namespace blender::gpu {

class TimestampPool {
 public:
  virtual ~TimestampPool() = default;
  virtual int timestamp_write() { return -1; }
  virtual void timestamp_resolve() {}
  virtual bool timestamp_duration(int, int, double &) const { return false; }

  static std::unique_ptr<TimestampPool> create();
};

}  // namespace blender::gpu
