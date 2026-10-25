/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "gpu_shader_diagnostics.hh"

#ifdef GPU_DIAGNOSTICS_STANDALONE
#  include <stdexcept>
#  define TEST(suite, name) static void name()
#  define EXPECT_EQ(a, b) \
    do { \
      if ((a) != (b)) { \
        throw std::runtime_error("Diagnostic range test failed at line " + \
                                 std::to_string(__LINE__)); \
      } \
    } while (false)
#  define ASSERT_EQ EXPECT_EQ
#else
#  include "testing/testing.h"
#endif

namespace blender::gpu::tests {

static const std::vector<ShaderDiagnosticSource> sources = {
    {"script_expr_17_77", "result", "float"}};

TEST(shader_diagnostics, evaluation_only)
{
  const auto ranges = shader_diagnostics::evaluation_ranges(
      "void script_expr_17_77(out float result)\n{\n  float input = missing_input;\n"
      "  result = float(\n    missing_expression + 1.0\n  );\n}\n",
      sources);
  ASSERT_EQ(ranges.size(), 1);
  EXPECT_EQ(ranges[0].first_line, 4);
  EXPECT_EQ(ranges[0].last_line, 6);
}

TEST(shader_diagnostics, malformed_external_scope)
{
  EXPECT_EQ(shader_diagnostics::evaluation_ranges(
                "void broken() {\nvoid script_expr_17_77(out float result) {result = float(x);}",
                sources)
                .size(),
            0);
  EXPECT_EQ(shader_diagnostics::evaluation_ranges(
                "float broken = \nvoid script_expr_17_77(out float result) {result = float(x);}",
                sources)
                .size(),
            0);
  EXPECT_EQ(shader_diagnostics::evaluation_ranges(
                "void script_expr_17_77(out float result) {result = float(x);", sources)
                .size(),
            0);
}

TEST(shader_diagnostics, comments_and_strings)
{
  const auto ranges = shader_diagnostics::evaluation_ranges(
      "// void script_expr_17_77(out float result) { result = float(fake); }\n"
      "/* { */\nvoid script_expr_17_77(out float result)\n{\n result = float(real);\n}",
      sources);
  ASSERT_EQ(ranges.size(), 1);
  EXPECT_EQ(ranges[0].first_line, 5);
  EXPECT_EQ(ranges[0].last_line, 5);
}

TEST(shader_diagnostics, result_adapter_excluded)
{
  const std::vector<ShaderDiagnosticSource> vector_source = {
      {"script_expr_4_99", "_script_expr_value", "vec4"}};
  const auto ranges = shader_diagnostics::evaluation_ranges(
      "void script_expr_4_99(out vec3 result, out float W)\n{\n"
      " vec4 _script_expr_value = vec4(missing);\n"
      " result = _script_expr_value.xyz;\n W = _script_expr_value.w;\n}\n",
      vector_source);
  ASSERT_EQ(ranges.size(), 1);
  EXPECT_EQ(ranges[0].first_line, 3);
  EXPECT_EQ(ranges[0].last_line, 3);
}

TEST(shader_diagnostics, ambiguous_macro_expansion)
{
  EXPECT_EQ(
      shader_diagnostics::evaluation_ranges(
          "void script_expr_17_77(out float result) { result = changed_constructor(x); }", sources)
          .size(),
      0);
  EXPECT_EQ(shader_diagnostics::evaluation_ranges(
                "void script_expr_17_77(out float result) { result = float((x); }", sources)
                .size(),
            0);
}

}  // namespace blender::gpu::tests

#ifdef GPU_DIAGNOSTICS_STANDALONE
int main()
{
  using namespace blender::gpu::tests;
  evaluation_only();
  malformed_external_scope();
  comments_and_strings();
  result_adapter_excluded();
  ambiguous_macro_expansion();
}
#endif
