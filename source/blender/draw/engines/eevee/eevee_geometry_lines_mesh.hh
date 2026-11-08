/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <memory>
#include <string>

#include "eevee_geometry_lines_graph.hh"

namespace blender::draw {
class ObjectRef;
}
namespace blender::eevee {
class VelocityModule;
}
namespace blender::eevee::geometry_lines {

class GeometryBuilder {
  struct Impl;
  std::unique_ptr<Impl> impl_;

 public:
  GeometryBuilder();
  ~GeometryBuilder();
  void begin(const Graph &graph, bool images_changed, VelocityModule *motion = nullptr);
  void add(const draw::ObjectRef &object);
  std::shared_ptr<const Geometry> finish();
  void clear();
  const std::string &error() const;
};

}  // namespace blender::eevee::geometry_lines
