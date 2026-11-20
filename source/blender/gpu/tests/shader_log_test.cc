/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifdef GPU_DIAGNOSTICS_STANDALONE
#  include "shader_log_fixture.hh"
#else
#  include "gl_shader.hh"
#  include "testing/testing.h"
#endif

namespace blender::gpu::tests {

static constexpr const char *source = "void main()\n{\n float x;\n x = float(\n missing);\n}";

static void expect_location(const char *log, int source_index, int row, int column)
{
  GLLogParser parser;
  GPULogItem item;
  const char *message = parser.parse_line(source, log, item);
  EXPECT_EQ(item.severity, Severity::Error);
  EXPECT_EQ(item.cursor.source, source_index);
  EXPECT_EQ(item.cursor.row, row);
  EXPECT_EQ(item.cursor.column, column);
  EXPECT_EQ(std::string(message).find("missing") != std::string::npos, true);
}

TEST(shader_log, source_row)
{
  expect_location("ERROR: 0:5: error: missing", 0, 5, -1);
  expect_location("0(5) : error C1503: missing\n", 0, 5, -1);
  expect_location("ERROR: 2:5: missing\n", 2, 5, -1);
}

TEST(shader_log, three_fields)
{
  expect_location("0:5(2): error: missing\n", 0, 5, 2);
  expect_location("ERROR: 0:5:2: missing\n", 0, 5, 2);
  expect_location("2:5(2): error: missing\n", 2, 5, 2);
}

TEST(shader_log, missing_and_invalid_location)
{
  expect_location("ERROR: missing", -1, -1, -1);
  expect_location("ERROR: 0:0: missing", 0, 0, -1);
  expect_location("ERROR: 0:999999: missing", 0, 999999, -1);
  expect_location("ERROR: 0:6: missing", 0, 6, -1);
}

TEST(shader_log, warning_severity)
{
  GLLogParser parser;
  GPULogItem item;
  parser.parse_line(source, "WARNING: 0:5: warning: missing", item);
  EXPECT_EQ(item.severity, Severity::Warning);
  EXPECT_EQ(item.cursor.row, 5);
}

}  // namespace blender::gpu::tests

#ifdef GPU_DIAGNOSTICS_STANDALONE
int main()
{
  using namespace blender;
  using namespace blender::gpu::tests;
  for (auto device : {GPU_DEVICE_NVIDIA, GPU_DEVICE_ATI, GPU_DEVICE_INTEL}) {
    for (auto os : {GPU_OS_WIN, GPU_OS_UNIX}) {
      test_device = device;
      test_os = os;
      source_row();
      three_fields();
      missing_and_invalid_location();
      warning_severity();
    }
  }
}
#endif
