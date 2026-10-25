/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace blender::gpu {

/** Immutable compiler metadata. Never contains node or material pointers. */
struct ShaderDiagnosticSource {
  std::string function_name;
  std::string evaluation_target;
  std::string constructor;
};

struct ShaderDiagnostic {
  std::string function_name;
  std::string message;
};

namespace shader_diagnostics {

struct EvaluationRange {
  size_t source;
  int first_line;
  int last_line;
};

struct Token {
  std::string_view text;
  int line;
};

/** Only used on compilation failure, on the exact text submitted to the backend. */
inline std::vector<EvaluationRange> evaluation_ranges(
    std::string_view code, const std::vector<ShaderDiagnosticSource> &sources)
{
  std::vector<Token> tokens;
  int line = 1;
  for (size_t i = 0; i < code.size();) {
    const char c = code[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      line += c == '\n';
      i++;
      continue;
    }
    if (c == '#') {
      while (i < code.size()) {
        if (code[i] == '\n') {
          line++;
          if (i == 0 || code[i - 1] != '\\') {
            i++;
            break;
          }
        }
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
      while (i < code.size() && code[i] != '\n') {
        i++;
      }
      continue;
    }
    if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
      i += 2;
      while (i + 1 < code.size() && !(code[i] == '*' && code[i + 1] == '/')) {
        line += code[i++] == '\n';
      }
      if (i + 1 >= code.size()) {
        return {};
      }
      i += 2;
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      i++;
      while (i < code.size() && code[i] != quote) {
        if (code[i] == '\\' && i + 1 < code.size()) {
          i++;
        }
        line += code[i++] == '\n';
      }
      if (i == code.size()) {
        return {};
      }
      i++;
      continue;
    }
    const size_t start = i++;
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      while (i < code.size() &&
             (std::isalnum(static_cast<unsigned char>(code[i])) || code[i] == '_'))
      {
        i++;
      }
    }
    tokens.push_back({code.substr(start, i - start), line});
  }

  std::vector<EvaluationRange> ranges;
  int depth = 0;
  int parentheses = 0;
  int brackets = 0;
  for (size_t i = 0; i < tokens.size(); i++) {
    const auto text = tokens[i].text;
    if (text == "void" && depth == 0 && parentheses == 0 && brackets == 0 &&
        i + 2 < tokens.size() &&
        (i == 0 || tokens[i - 1].text == ";" || tokens[i - 1].text == "}"))
    {
      for (size_t source = 0; source < sources.size(); source++) {
        if (tokens[i + 1].text != sources[source].function_name || tokens[i + 2].text != "(") {
          continue;
        }
        size_t body = i + 2;
        int signature_depth = 0;
        do {
          signature_depth += tokens[body].text == "(";
          signature_depth -= tokens[body].text == ")";
          body++;
        } while (body < tokens.size() && signature_depth > 0);
        if (body == tokens.size() || signature_depth != 0 || tokens[body].text != "{") {
          continue;
        }
        size_t body_end = body + 1;
        int closing_depth = 1;
        while (body_end < tokens.size() && closing_depth > 0) {
          closing_depth += tokens[body_end].text == "{";
          closing_depth -= tokens[body_end].text == "}";
          body_end++;
        }
        if (closing_depth != 0) {
          continue;
        }
        int body_depth = 1;
        for (size_t j = body + 1; j < body_end && body_depth > 0; j++) {
          body_depth += tokens[j].text == "{";
          body_depth -= tokens[j].text == "}";
          if (body_depth != 1 || j + 3 >= tokens.size() ||
              tokens[j].text != sources[source].evaluation_target || tokens[j + 1].text != "=" ||
              tokens[j + 2].text != sources[source].constructor || tokens[j + 3].text != "(")
          {
            continue;
          }
          int expression_depth = 0;
          int indexing_depth = 0;
          for (size_t end = j + 3; end < tokens.size(); end++) {
            expression_depth += tokens[end].text == "(";
            expression_depth -= tokens[end].text == ")";
            indexing_depth += tokens[end].text == "[";
            indexing_depth -= tokens[end].text == "]";
            if (expression_depth < 0 || indexing_depth < 0 || tokens[end].text == "{" ||
                tokens[end].text == "}")
            {
              break;
            }
            if (tokens[end].text == ";") {
              if (expression_depth == 0 && indexing_depth == 0) {
                ranges.push_back({source, tokens[j + 2].line, tokens[end].line});
              }
              break;
            }
          }
        }
      }
    }
    depth += text == "{";
    depth -= text == "}";
    parentheses += text == "(";
    parentheses -= text == ")";
    brackets += text == "[";
    brackets -= text == "]";
    if (depth < 0 || parentheses < 0 || brackets < 0) {
      break;
    }
  }
  return ranges;
}

}  // namespace shader_diagnostics
}  // namespace blender::gpu
