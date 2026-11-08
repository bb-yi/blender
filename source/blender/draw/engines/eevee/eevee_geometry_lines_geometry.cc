/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_geometry_lines_geometry.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

namespace blender::eevee::geometry_lines {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Box {
  double3 lo = double3(std::numeric_limits<double>::max());
  double3 hi = double3(-std::numeric_limits<double>::max());
  void add(double3 p)
  {
    for (int i = 0; i < 3; i++) {
      lo[i] = std::min(lo[i], p[i]);
      hi[i] = std::max(hi[i], p[i]);
    }
  }
  bool overlaps(const Box &other) const
  {
    for (int i = 0; i < 3; i++) {
      if (lo[i] > other.hi[i] + 1e-9 || hi[i] < other.lo[i] - 1e-9) {
        return false;
      }
    }
    return true;
  }
};

/** Compact deterministic BVH. Leaf queries allocate no per-edge containers. */
class BVH {
  struct Node {
    Box box;
    int begin = 0, end = 0, left = -1, right = -1;
  };
  std::vector<Box> boxes_;
  std::vector<int> indices_;
  std::vector<Node> nodes_;
  int dimensions_ = 3;
  const std::atomic<bool> *cancel_ = nullptr;
  int build(int begin, int end)
  {
    const int index = int(nodes_.size());
    nodes_.emplace_back();
    Box box;
    for (int i = begin; i < end; i++) {
      if ((i & 1023) == 0 && cancel_ && cancel_->load(std::memory_order_relaxed)) { return index; }
      box.add(boxes_[indices_[i]].lo);
      box.add(boxes_[indices_[i]].hi);
    }
    nodes_[index].box = box;
    nodes_[index].begin = begin;
    nodes_[index].end = end;
    if (end - begin > 12) {
      const double3 size = box.hi - box.lo;
      const int axis = dimensions_ == 2 ? (size.x > size.y ? 0 : 1) :
                         size.x > size.y ? (size.x > size.z ? 0 : 2) : (size.y > size.z ? 1 : 2);
      const int mid = (begin + end) / 2;
      std::nth_element(indices_.begin() + begin, indices_.begin() + mid, indices_.begin() + end,
                       [&](int a, int b) {
        const double ca = boxes_[a].lo[axis] + boxes_[a].hi[axis];
        const double cb = boxes_[b].lo[axis] + boxes_[b].hi[axis];
        return ca == cb ? a < b : ca < cb;
      });
      const int left = build(begin, mid);
      const int right = build(mid, end);
      nodes_[index].left = left;
      nodes_[index].right = right;
    }
    return index;
  }

 public:
  explicit BVH(std::vector<Box> boxes, int dimensions = 3, const std::atomic<bool> *cancel = nullptr)
      : boxes_(std::move(boxes)), dimensions_(dimensions), cancel_(cancel)
  {
    indices_.resize(boxes_.size());
    std::iota(indices_.begin(), indices_.end(), 0);
    nodes_.reserve(boxes_.size() * 2);
    if (!boxes_.empty()) {
      build(0, int(boxes_.size()));
    }
  }
  template<class Fn> void query(const Box &box, Fn fn) const
  {
    if (nodes_.empty()) {
      return;
    }
    std::array<int, 128> stack;
    int count = 1;
    stack[0] = 0;
    while (count) {
      if (cancel_ && cancel_->load(std::memory_order_relaxed)) { return; }
      const Node &node = nodes_[stack[--count]];
      if (!node.box.overlaps(box)) {
        continue;
      }
      if (node.left >= 0) {
        stack[count++] = node.right;
        stack[count++] = node.left;
      }
      else {
        for (int i = node.begin; i < node.end; i++) {
          const int item = indices_[i];
          if (boxes_[item].overlaps(box)) {
            fn(item);
          }
        }
      }
    }
  }
};

uint64_t mix_id(uint64_t a, uint64_t b)
{
  return (a ^ (b + 0x9e3779b97f4a7c15ULL + (a << 6) + (a >> 2))) | 1;
}

bool point_in_triangle(double3 p, const std::array<double3, 3> &tri, double tolerance)
{
  const double3 u = tri[1] - tri[0], v = tri[2] - tri[0], w = p - tri[0];
  const double uu = math::dot(u, u), uv = math::dot(u, v), vv = math::dot(v, v);
  const double wu = math::dot(w, u), wv = math::dot(w, v), den = uu * vv - uv * uv;
  if (std::abs(den) < 1e-28) {
    return false;
  }
  const double a = (wu * vv - wv * uv) / den, b = (wv * uu - wu * uv) / den;
  return a >= -tolerance && b >= -tolerance && a + b <= 1 + tolerance;
}

bool intersect_triangles(const std::array<double3, 3> &a,
                          const std::array<double3, 3> &b,
                          double3 &start,
                          double3 &end)
{
  const double3 na = math::normalize(math::cross(a[1] - a[0], a[2] - a[0]));
  const double3 nb = math::normalize(math::cross(b[1] - b[0], b[2] - b[0]));
  if (math::length_squared(math::cross(na, nb)) < 1e-18) {
    return false; /* Coplanar geometry has no unique intersection curve. */
  }
  double scale = 1;
  for (int i = 0; i < 3; i++) {
    scale = std::max(scale, math::length(a[i] - b[0]));
  }
  const double eps = scale * 1e-10;
  std::array<double3, 12> points;
  int point_count = 0;
  auto cut = [&](const std::array<double3, 3> &from,
                 const std::array<double3, 3> &to,
                 double3 normal) {
    double d[3];
    for (int i = 0; i < 3; i++) {
      d[i] = math::dot(from[i] - to[0], normal);
    }
    if ((d[0] > eps && d[1] > eps && d[2] > eps) ||
        (d[0] < -eps && d[1] < -eps && d[2] < -eps)) {
      return;
    }
    for (int i = 0; i < 3; i++) {
      const int j = (i + 1) % 3;
      if (std::abs(d[i]) <= eps && point_in_triangle(from[i], to, 1e-9)) {
        points[point_count++] = from[i];
      }
      if ((d[i] < -eps && d[j] > eps) || (d[j] < -eps && d[i] > eps)) {
        const double3 p = from[i] + (from[j] - from[i]) * (d[i] / (d[i] - d[j]));
        if (point_in_triangle(p, to, 1e-9)) {
          points[point_count++] = p;
        }
      }
    }
  };
  cut(a, b, nb);
  cut(b, a, na);
  double maximum = eps * eps;
  bool found = false;
  for (int i = 0; i < point_count; i++) {
    for (int j = i + 1; j < point_count; j++) {
      const double distance = math::length_squared(points[i] - points[j]);
      if (distance > maximum) {
        maximum = distance;
        start = points[i];
        end = points[j];
        found = true;
      }
    }
  }
  return found;
}

struct Interval {
  double a, b;
};

/** Restrict [lo, hi] by the affine inequality a + t * b >= 0. */
bool restrict_interval(double a, double b, double &lo, double &hi)
{
  if (std::abs(b) < 1e-20) {
    return a >= 0;
  }
  const double root = -a / b;
  if (b > 0) {
    lo = std::max(lo, root);
  }
  else {
    hi = std::min(hi, root);
  }
  return hi > lo + 1e-12;
}

double4 homogeneous(const double4x4 &matrix, double3 p)
{
  return matrix * double4(p.x, p.y, p.z, 1.0);
}

double3 screen_position(double4 p, const Camera &camera)
{
  return double3((p.x / p.w * 0.5 + 0.5) * camera.extent.x,
                 (p.y / p.w * 0.5 + 0.5) * camera.extent.y,
                 p.z / p.w);
}

struct ClipVertex {
  double4 position;
  double2 uv;
};

double clip_distance(const double4 &p, int plane, const Camera &camera)
{
  switch (plane) {
    case 0: return p.w - 1e-10;
    case 1: return p.w + p.z;
    case 2: return p.w - p.z;
    case 3: return p.w * (1 + 2 * camera.clip_margin / camera.extent.x) + p.x;
    case 4: return p.w * (1 + 2 * camera.clip_margin / camera.extent.x) - p.x;
    case 5: return p.w * (1 + 2 * camera.clip_margin / camera.extent.y) + p.y;
    case 6: return p.w * (1 + 2 * camera.clip_margin / camera.extent.y) - p.y;
    default: return 1;
  }
}

struct ProjectedTriangle {
  std::array<double3, 3> p;
  std::array<double2, 3> uv;
  std::array<double, 3> inverse_w;
  int face;
  Box box;
  double inverse_area;

  double3 barycentric(double3 point) const
  {
    const double x = point.x - p[0].x, y = point.y - p[0].y;
    const double u = (x * (p[2].y - p[0].y) - y * (p[2].x - p[0].x)) * inverse_area;
    const double v = ((p[1].x - p[0].x) * y - (p[1].y - p[0].y) * x) * inverse_area;
    return double3(1 - u - v, u, v);
  }
  double2 texture_uv(double3 point) const
  {
    double3 weights = barycentric(point);
    for (int i = 0; i < 3; i++) {
      weights[i] *= inverse_w[i];
    }
    const double sum = weights.x + weights.y + weights.z;
    return (uv[0] * weights.x + uv[1] * weights.y + uv[2] * weights.z) / sum;
  }
};

std::vector<ProjectedTriangle> project_triangles(const Geometry &geometry, const Camera &camera,
                                                const std::atomic<bool> &cancel)
{
  std::vector<ProjectedTriangle> result;
  result.reserve(geometry.triangles.size());
  std::vector<double4> projected_vertices;
  projected_vertices.reserve(geometry.positions.size());
  for (double3 position : geometry.positions) {
    if (cancel.load(std::memory_order_relaxed)) { return {}; }
    projected_vertices.push_back(homogeneous(camera.projection, position));
  }
  ClipVertex buffers[2][16];
  for (const Triangle &tri : geometry.triangles) {
    if (cancel.load(std::memory_order_relaxed)) { return {}; }
    ClipVertex *polygon = buffers[0], *clipped = buffers[1];
    int count = 3;
    for (int i = 0; i < 3; i++) {
      polygon[i] = {projected_vertices[tri.vertices[i]], tri.uv[i]};
    }
    for (int plane = 0; plane < 7 && count; plane++) {
      int inside = 0;
      for (int i = 0; i < count; i++) { inside += clip_distance(polygon[i].position, plane, camera) >= 0; }
      if (inside == count) { continue; }
      if (!inside) { count = 0; break; }
      int clipped_count = 0;
      ClipVertex previous = polygon[count - 1];
      double d0 = clip_distance(previous.position, plane, camera);
      for (int i = 0; i < count; i++) {
        const ClipVertex &current = polygon[i];
        const double d1 = clip_distance(current.position, plane, camera);
        if ((d0 >= 0) != (d1 >= 0)) {
          const double t = d0 / (d0 - d1);
          clipped[clipped_count++] = {previous.position + (current.position - previous.position) * t,
                                     previous.uv + (current.uv - previous.uv) * t};
        }
        if (d1 >= 0) {
          clipped[clipped_count++] = current;
        }
        previous = current;
        d0 = d1;
      }
      std::swap(polygon, clipped);
      count = clipped_count;
    }
    for (int i = 1; i + 1 < count; i++) {
      const ClipVertex vertices[] = {polygon[0], polygon[i], polygon[i + 1]};
      ProjectedTriangle projected;
      projected.face = tri.face;
      for (int j = 0; j < 3; j++) {
        projected.p[j] = screen_position(vertices[j].position, camera);
        projected.uv[j] = vertices[j].uv;
        projected.inverse_w[j] = 1 / vertices[j].position.w;
        projected.box.add(projected.p[j]);
      }
      const double3 u = projected.p[1] - projected.p[0], v = projected.p[2] - projected.p[0];
      const double area = u.x * v.y - u.y * v.x;
      if (std::abs(area) < 1e-12) {
        continue;
      }
      projected.inverse_area = 1 / area;
      projected.box.lo.z = -1e100;
      projected.box.hi.z = 1e100;
      result.push_back(projected);
    }
  }
  return result;
}

bool triangle_interval(const ProjectedTriangle &tri,
                        double3 a,
                        double3 b,
                        bool depth,
                        Interval &interval)
{
  const double3 ba = tri.barycentric(a), bb = tri.barycentric(b);
  double lo = 0, hi = 1;
  for (int i = 0; i < 3; i++) {
    if (!restrict_interval(ba[i] + 1e-10, bb[i] - ba[i], lo, hi)) {
      return false;
    }
  }
  if (depth) {
    const double za = ba.x * tri.p[0].z + ba.y * tri.p[1].z + ba.z * tri.p[2].z;
    const double zb = bb.x * tri.p[0].z + bb.y * tri.p[1].z + bb.z * tri.p[2].z;
    const double bias = 2e-7;
    if (!restrict_interval(a.z - za - bias, (b.z - zb) - (a.z - za), lo, hi)) {
      return false;
    }
  }
  interval = {lo, hi};
  return hi > lo;
}

void merge_intervals(std::vector<Interval> &intervals)
{
  std::sort(intervals.begin(), intervals.end(), [](auto a, auto b) {
    return a.a == b.a ? a.b < b.b : a.a < b.a;
  });
  size_t count = 0;
  for (const Interval interval : intervals) {
    if (count && interval.a <= intervals[count - 1].b + 1e-10) {
      intervals[count - 1].b = std::max(intervals[count - 1].b, interval.b);
    }
    else {
      intervals[count++] = interval;
    }
  }
  intervals.resize(count);
}

bool contains(const std::vector<Interval> &intervals, double t)
{
  const auto found = std::upper_bound(intervals.begin(), intervals.end(), t,
                                      [](double value, Interval interval) { return value < interval.a; });
  return found != intervals.begin() && (found - 1)->b >= t;
}

bool resolve_rules(const Plan &plan,
                    const std::vector<int> &matches,
                    std::vector<int> &output)
{
  if (plan.rule >= 0) {
    if (std::find(matches.begin(), matches.end(), plan.rule) != matches.end()) {
      output.push_back(plan.rule);
      return true;
    }
    return false;
  }
  bool claimed = false;
  for (const Plan &child : plan.children) {
    const bool matched = resolve_rules(child, matches, output);
    claimed |= matched;
    if (matched && !plan.overlay) {
      break;
    }
  }
  return claimed;
}

int choose_type(uint32_t mask, uint32_t types, bool contour, bool interior, bool contour_priority)
{
  if (contour && contour_priority) {
    const int kind = interior ? 1 : 0;
    return mask & (1 << kind) ? kind : -1;
  }
  if ((mask & types) & Marked) {
    return 7;
  }
  if (contour && (mask & (Outline | ObjectContour))) {
    const int kind = interior ? 1 : 0;
    return mask & (1 << kind) ? kind : -1;
  }
  for (int type : {2, 4, 3, 5, 6}) {
    if ((mask & types) & (1 << type)) {
      return type;
    }
  }
  return -1;
}

}  // namespace

void AlphaMask::build_hierarchy()
{
  hierarchy.clear();
  if (alpha.empty() || minimum >= threshold || maximum < threshold) { return; }
  /* Start at 2x2 texels: the original alpha buffer already supplies the finest level. */
  int2 previous_extent(width, height);
  while (previous_extent.x > 1 || previous_extent.y > 1) {
    Level level;
    level.extent = (previous_extent + int2(1)) / 2;
    level.bounds.resize(size_t(level.extent.x) * level.extent.y);
    for (int y = 0; y < level.extent.y; y++) {
      for (int x = 0; x < level.extent.x; x++) {
        float2 range(1, 0);
        for (int dy = 0; dy < 2; dy++) {
          for (int dx = 0; dx < 2; dx++) {
            const int xx = x * 2 + dx, yy = y * 2 + dy;
            if (xx >= previous_extent.x || yy >= previous_extent.y) { continue; }
            const size_t index = size_t(yy) * previous_extent.x + xx;
            const float2 child = hierarchy.empty() ? float2(alpha[index]) : hierarchy.back().bounds[index];
            range.x = std::min(range.x, child.x);
            range.y = std::max(range.y, child.y);
          }
        }
        level.bounds[size_t(y) * level.extent.x + x] = range;
      }
    }
    previous_extent = level.extent;
    hierarchy.push_back(std::move(level));
  }
}

int AlphaMask::coverage(double2 a, double2 b) const
{
  if (minimum >= threshold) { return 1; }
  if (maximum < threshold) { return 0; }
  if (hierarchy.empty() || !std::isfinite(a.x + a.y + b.x + b.y)) { return -1; }
  /* Repeat boundaries require examining both ends of the image. Subdivision handles these
   * rare intervals; never infer a clear/solid interval from only one wrapped endpoint. */
  if (std::floor(a.x) != std::floor(b.x) || std::floor(a.y) != std::floor(b.y)) { return -1; }
  a -= double2(std::floor(a.x), std::floor(a.y));
  b -= double2(std::floor(b.x), std::floor(b.y));
  int2 lo(int(std::floor(std::min(a.x, b.x) * width - 0.5)),
          int(std::floor(std::min(a.y, b.y) * height - 0.5)));
  int2 hi(int(std::floor(std::max(a.x, b.x) * width - 0.5)) + 1,
          int(std::floor(std::max(a.y, b.y) * height - 0.5)) + 1);
  if (lo.x < 0 || lo.y < 0 || hi.x >= width || hi.y >= height) { return -1; }
  int mip = 0;
  while (mip + 1 < int(hierarchy.size()) &&
         std::max(hi.x - lo.x, hi.y - lo.y) > (2 << mip)) { mip++; }
  lo = lo / (2 << mip);
  hi = hi / (2 << mip);
  const Level &level = hierarchy[mip];
  float2 range(1, 0);
  for (int y = lo.y; y <= hi.y; y++) {
    for (int x = lo.x; x <= hi.x; x++) {
      const float2 child = level.bounds[size_t(y) * level.extent.x + x];
      range.x = std::min(range.x, child.x);
      range.y = std::max(range.y, child.y);
    }
  }
  return range.x >= threshold ? 1 : range.y < threshold ? 0 : -1;
}

bool AlphaMask::sample(double2 uv) const
{
  if (width <= 0 || height <= 0 || alpha.empty()) {
    return true;
  }
  const double x = (uv.x - std::floor(uv.x)) * width - 0.5;
  const double y = (uv.y - std::floor(uv.y)) * height - 0.5;
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  auto pixel = [&](int px, int py) {
    return alpha[((py % height + height) % height) * width + (px % width + width) % width];
  };
  const double fx = x - ix, fy = y - iy;
  return ((1 - fx) * pixel(ix, iy) + fx * pixel(ix + 1, iy)) * (1 - fy) +
             ((1 - fx) * pixel(ix, iy + 1) + fx * pixel(ix + 1, iy + 1)) * fy >= threshold;
}

std::vector<IntersectionEdge> find_intersections(const Geometry &geometry,
                                                Statistics &statistics,
                                                const std::atomic<bool> &cancel)
{
  const auto start = Clock::now();
  std::vector<Box> boxes;
  boxes.reserve(geometry.triangles.size());
  for (const Triangle &tri : geometry.triangles) {
    Box box;
    for (int axis = 0; axis < 3; axis++) {
      box.add(geometry.positions[tri.vertices[axis]]);
    }
    boxes.push_back(box);
  }
  BVH bvh(boxes, 3, &cancel);
  if (cancel.load(std::memory_order_relaxed)) { return {}; }
  std::vector<IntersectionEdge> result;
  auto process_range = [&](IndexRange range, std::vector<IntersectionEdge> &edges, uint64_t &tests) {
    uint64_t local_tests = 0;
    for (const int64_t index : range) {
      if (cancel.load(std::memory_order_relaxed)) { break; }
      const int i = int(index);
      const Triangle &a = geometry.triangles[i];
      bvh.query(boxes[i], [&](int j) {
        if (j <= i) {
          return;
        }
        const Triangle &b = geometry.triangles[j];
        if (a.face == b.face) {
          return;
        }
        int shared_vertices = 0;
        for (int av = 0; av < 3; av++) {
          for (int bv = 0; bv < 3; bv++) {
            shared_vertices += a.vertices[av] == b.vertices[bv];
          }
        }
        if (shared_vertices >= 2) { return; }
        const Surface &sa = geometry.surfaces[geometry.faces[a.face].surface];
        const Surface &sb = geometry.surfaces[geometry.faces[b.face].surface];
        if (sa.rules.empty() && sb.rules.empty()) {
          return;
        }
        local_tests++;
        double3 from, to;
        if (intersect_triangles({geometry.positions[a.vertices[0]], geometry.positions[a.vertices[1]], geometry.positions[a.vertices[2]]},
                                {geometry.positions[b.vertices[0]], geometry.positions[b.vertices[1]], geometry.positions[b.vertices[2]]}, from, to))
        {
          edges.push_back({from, to, int2(a.face, b.face),
                            mix_id(geometry.faces[a.face].identity, geometry.faces[b.face].identity), int2(i, j)});
        }
      });
    }
    tests += local_tests;
  };
  const int64_t work_count = int64_t(geometry.triangles.size());
  if (work_count < 4096) {
    process_range(IndexRange(work_count), result, statistics.intersection_tests);
  }
  else {
    struct Partial {
      std::vector<IntersectionEdge> edges;
      uint64_t tests = 0;
    };
    constexpr int64_t batch_size = 1024;
    const int64_t batch_count = (work_count + batch_size - 1) / batch_size;
    std::vector<Partial> batches(batch_count);
    threading::parallel_for(IndexRange(batch_count), 1, [&](IndexRange range) {
      for (const int64_t batch : range) {
        const int64_t begin = batch * batch_size;
        process_range(IndexRange(begin, std::min(batch_size, work_count - begin)),
                       batches[batch].edges, batches[batch].tests);
      }
    });
    if (cancel.load(std::memory_order_relaxed)) { return {}; }
    size_t count = 0;
    for (const Partial &batch : batches) { count += batch.edges.size(); }
    result.reserve(count);
    /* The original triangle/query order is also the tie order used by stroke
     * matching. Parallel workers must not reorder intersection pieces. */
    for (Partial &batch : batches) {
      if (cancel.load(std::memory_order_relaxed)) { return {}; }
      statistics.intersection_tests += batch.tests;
      result.insert(result.end(), batch.edges.begin(), batch.edges.end());
      std::vector<IntersectionEdge>().swap(batch.edges);
    }
  }
  statistics.intersection_ms = elapsed(start);
  return result;
}

Result evaluate(const Geometry &geometry,
                const Camera &camera,
                const Plan &plan,
                const std::vector<Rule> &rules,
                const std::vector<IntersectionEdge> &intersections,
                const std::atomic<bool> &cancel,
                bool connect)
{
  Result result;
  result.extent = camera.extent;
  const auto start = Clock::now();
  const auto triangles = camera.quick ? std::vector<ProjectedTriangle>() : project_triangles(geometry, camera, cancel);
  if (cancel.load()) { result.cancelled = true; return result; }
  std::vector<Box> boxes;
  boxes.reserve(triangles.size());
  for (const auto &tri : triangles) {
    boxes.push_back(tri.box);
  }
  BVH bvh(std::move(boxes), 2, &cancel);
  if (cancel.load()) { result.cancelled = true; return result; }
  std::vector<int> face_starts, face_triangles;
  const bool source_masks = !camera.quick && std::any_of(geometry.surfaces.begin(), geometry.surfaces.end(),
                                        [](const Surface &s) { return s.mask && !s.rules.empty(); });
  {
    face_starts.resize(geometry.faces.size() + 1, 0);
    for (const Triangle &triangle : geometry.triangles) { face_starts[triangle.face + 1]++; }
    std::partial_sum(face_starts.begin(), face_starts.end(), face_starts.begin());
    auto cursor = face_starts;
    face_triangles.resize(geometry.triangles.size());
    for (int i = 0; i < int(geometry.triangles.size()); i++) {
      face_triangles[cursor[geometry.triangles[i].face]++] = i;
    }
  }
  auto attribute_point = [&](int face, double3 point, double inverse_w) {
    AttributePoint result;
    result.inverse_w = float(inverse_w);
    for (int j = face_starts[face]; j < face_starts[face + 1]; j++) {
      const int index = face_triangles[j];
      const Triangle &triangle = geometry.triangles[index];
      const double3 a = geometry.positions[triangle.vertices.x];
      const double3 u = geometry.positions[triangle.vertices.y] - a;
      const double3 v = geometry.positions[triangle.vertices.z] - a;
      const double3 p = point - a;
      const double uu = math::dot(u, u), uv = math::dot(u, v), vv = math::dot(v, v);
      const double denominator = uu * vv - uv * uv;
      if (denominator <= std::max(uu * vv, 1e-30) * 1e-14) { continue; }
      const double plane_distance = std::abs(math::dot(p, math::cross(u, v)));
      if (plane_distance > std::sqrt(denominator * std::max(uu, vv)) * 1e-6 + 1e-12) { continue; }
      const double pu = math::dot(p, u), pv = math::dot(p, v);
      const double x = (pu * vv - pv * uv) / denominator;
      const double y = (pv * uu - pu * uv) / denominator;
      if (x >= -1e-6 && y >= -1e-6 && x + y <= 1.000001) {
        result.triangle = index;
        result.barycentric = float2(float(x), float(y));
        break;
      }
    }
    return result;
  };
  /* Each contiguous batch owns its scratch/output. Merge batches in source order below:
   * endpoint matching and equal-score choices must not depend on worker scheduling. */
  auto process_range = [&](IndexRange range, Result &result) {
    std::vector<Interval> covered, left, right, object_left, object_right, source_coverage;
    std::vector<double> boundaries;
    std::vector<int> matching, selected;
    auto process = [&](double3 world_a, double3 world_b, std::span<const int> faces,
                       uint32_t types, double normal_dot, uint64_t identity,
                       uint64_t vertex_a, uint64_t vertex_b, bool is_intersection, bool loose,
                       double3 previous_world_a, double3 previous_world_b,
                       double3 next_world_a, double3 next_world_b,
                       int seam_mode = 0, uint64_t seam_vertex_a = 0, uint64_t seam_vertex_b = 0,
                       int source_edge = -1) {
      const size_t process_segment_begin = result.segments.size();
      matching.clear();
      selected.clear();
      for (int face : faces) {
        const Surface &surface = geometry.surfaces[geometry.faces[face].surface];
        matching.insert(matching.end(), surface.rules.begin(), surface.rules.end());
      }
      if (!resolve_rules(plan, matching, selected)) {
        return;
      }
      if (seam_mode) {
        std::erase_if(selected, [&](int ri) {
          return seam_mode == 1 ? rules[ri].repair_seams : !rules[ri].repair_seams;
        });
        if (selected.empty()) { return; }
      }
      double4 ca = homogeneous(camera.projection, world_a), cb = homogeneous(camera.projection, world_b);
      double lo = 0, hi = 1;
      for (int plane = 0; plane < 7; plane++) {
        const double da = clip_distance(ca, plane, camera), db = clip_distance(cb, plane, camera);
        if (!restrict_interval(da, db - da, lo, hi)) {
          return;
        }
      }
      const double4 original_a = ca, original_b = cb;
      ca = original_a + (original_b - original_a) * lo;
      cb = original_a + (original_b - original_a) * hi;
      const double3 a = screen_position(ca, camera), b = screen_position(cb, camera);
      auto world_parameter = [&](double screen_t) {
        const double perspective_t = screen_t * ca.w / (cb.w * (1 - screen_t) + ca.w * screen_t);
        return lo + (hi - lo) * perspective_t;
      };
      const double length = std::hypot(b.x - a.x, b.y - a.y);
      /* Screen length is not a topology filter. A foreshortened connector can become
       * subpixel or project to a point while its two neighbours still form one stroke.
       * Dropping it adds two moving tapered caps as the camera crosses the threshold. */
      if (math::length_squared(world_b - world_a) < 1e-40) {
        return;
      }
      bool front = false, back = false, grazing = false;
      for (int face : faces) {
        const double3 to_camera = camera.perspective ? camera.position - world_a : -camera.forward;
        const double facing = math::dot(geometry.faces[face].normal, to_camera) /
                              std::max(math::length(to_camera), 1e-20);
        front |= facing > 1e-7;
        back |= facing < -1e-7;
        grazing |= std::abs(facing) <= 1e-7;
      }
      const bool open = faces.size() == 1;
      /* An exactly edge-on side has two projected boundary edges. Test both and let visibility
       * retain the front one; assigning a grazing face to "front" alone loses orthographic
       * cube/sphere outlines by emitting only their occluded rear rim. */
      const bool contour = !is_intersection && !loose && ((front && back) || grazing || open);
      if (contour) { types &= ~Smoothing; }
      bool candidate = false, need_scene_sides = false, need_object_sides = false;
      for (int ri : selected) {
        const Rule &rule = rules[ri];
        uint32_t visible_mask = rule.visible, hidden_mask = rule.hidden;
        if (open) {
          if (!rule.visible_open_outline) { visible_mask &= ~Outline; }
          if (!rule.visible_open_object) { visible_mask &= ~ObjectContour; }
          if (!rule.hidden_open_outline) { hidden_mask &= ~Outline; }
          if (!rule.hidden_open_object) { hidden_mask &= ~ObjectContour; }
        }
        const uint32_t mask = visible_mask | (rule.hidden_enabled ? hidden_mask : 0);
        const bool selected_contour = contour && (!open || rule.open_edges) && (mask & (Outline | ObjectContour));
        const double radians = 3.141592653589793 / 180;
        const bool selected_angle = !contour && faces.size() > 1 && (mask & Normal) &&
                                    normal_dot <= std::cos(rule.normal_min * radians) + 1e-10 &&
                                    normal_dot >= std::cos(rule.normal_max * radians) - 1e-10;
        candidate |= selected_contour || selected_angle ||
                     ((!rule.contour_priority || !contour) && (mask & types));
        need_scene_sides |= selected_contour && !rule.object_contours;
        need_object_sides |= selected_contour && rule.object_contours;
      }
      if (!candidate) { return; }
      const double inverse_length = 1.0 / std::max(length, 1e-20);
      /* Probe immediately beside the edge. A pixel-sized offset can cross a narrow
       * background slit and incorrectly classify its two boundaries as internal. */
      constexpr double side_probe_distance = double(0.01f);
      const double3 normal(-(b.y - a.y) * inverse_length * side_probe_distance,
                            (b.x - a.x) * inverse_length * side_probe_distance, 0);
      covered.clear();
      left.clear();
      right.clear();
      object_left.clear();
      object_right.clear();
      source_coverage.clear();
      result.statistics.candidates++;
      const uint64_t source_instance = geometry.surfaces[geometry.faces[faces.front()].surface].instance;
      auto append_surface_coverage = [&](const ProjectedTriangle &tri, const Surface &surface,
                                          double3 qa, double3 qb, Interval interval,
                                          std::vector<Interval> &out) {
        if (surface.occlusion != 2 || !surface.mask ||
            surface.mask->minimum >= surface.mask->threshold) {
          out.push_back(interval);
          return;
        }
        if (surface.mask->maximum < surface.mask->threshold) { return; }
        const int steps = std::max(1, int(std::ceil((interval.b - interval.a) * length * 4)));
        auto refine = [&](const auto &self, int first, int last) -> void {
          if (cancel.load(std::memory_order_relaxed)) { return; }
          const double x = interval.a + (interval.b - interval.a) * double(first) / steps;
          const double y = interval.a + (interval.b - interval.a) * double(last) / steps;
          int coverage = surface.mask->coverage(tri.texture_uv(qa + (qb - qa) * x),
                                                tri.texture_uv(qa + (qb - qa) * y));
          if (coverage < 0 && last - first == 1) {
            result.statistics.mask_tests++;
            coverage = surface.mask->sample(tri.texture_uv(qa + (qb - qa) * ((x + y) * 0.5))) ? 1 : 0;
          }
          if (coverage == 1) { out.push_back({x, y}); }
          else if (coverage < 0) {
            const int mid = first + (last - first) / 2;
            self(self, first, mid);
            self(self, mid, last);
          }
        };
        refine(refine, 0, steps);
      };
      auto query = [&](double3 qa, double3 qb, bool depth, std::vector<Interval> &out,
                       bool include_source = false, uint64_t exclude_instance = 0) {
        Box box;
        box.add(qa);
        box.add(qb);
        box.lo.z = -1e100;
        /* A triangle entirely behind both endpoints cannot occlude this interval.
         * Keep a conservative margin for the barycentric boundary tolerance. */
        box.hi.z = depth ? std::max(qa.z, qb.z) + 1e-6 : 1e100;
        bvh.query(box, [&](int index) {
          if (cancel.load(std::memory_order_relaxed)) { return; }
          const auto &tri = triangles[index];
          if (!include_source && std::find(faces.begin(), faces.end(), tri.face) != faces.end()) {
            return;
          }
          const Surface &surface = geometry.surfaces[geometry.faces[tri.face].surface];
          if (exclude_instance && surface.instance == exclude_instance) { return; }
          if (surface.occlusion == 1) {
            return;
          }
          Interval interval;
          result.statistics.triangle_tests++;
          if (!triangle_interval(tri, qa, qb, depth, interval)) {
            return;
          }
          append_surface_coverage(tri, surface, qa, qb, interval, out);
        });
        merge_intervals(out);
      };
      auto query_visible_object = [&](double3 qa, double3 qb, std::vector<Interval> &out) {
        Box box;
        box.add(qa);
        box.add(qb);
        box.lo.z = -1e100;
        box.hi.z = 1e100;
        std::vector<Interval> target_intervals, occluders;
        bvh.query(box, [&](int index) {
          if (cancel.load(std::memory_order_relaxed)) { return; }
          const ProjectedTriangle &tri = triangles[index];
          const Surface &surface = geometry.surfaces[geometry.faces[tri.face].surface];
          if (surface.instance != source_instance || surface.occlusion == 1) { return; }
          Interval interval;
          result.statistics.triangle_tests++;
          if (!triangle_interval(tri, qa, qb, false, interval)) { return; }
          target_intervals.clear();
          append_surface_coverage(tri, surface, qa, qb, interval, target_intervals);
          if (target_intervals.empty()) { return; }
          const double3 ba = tri.barycentric(qa), bb = tri.barycentric(qb);
          const double3 depth_a(qa.x, qa.y, ba.x * tri.p[0].z + ba.y * tri.p[1].z + ba.z * tri.p[2].z);
          const double3 depth_b(qb.x, qb.y, bb.x * tri.p[0].z + bb.y * tri.p[1].z + bb.z * tri.p[2].z);
          occluders.clear();
          query(depth_a, depth_b, true, occluders, true, source_instance);
          for (const Interval target : target_intervals) {
            double begin = target.a;
            for (const Interval blocker : occluders) {
              if (blocker.b <= begin) { continue; }
              if (blocker.a >= target.b) { break; }
              if (blocker.a > begin) { out.push_back({begin, std::min(blocker.a, target.b)}); }
              begin = std::max(begin, blocker.b);
              if (begin >= target.b) { break; }
            }
            if (begin < target.b) { out.push_back({begin, target.b}); }
          }
        });
        merge_intervals(out);
      };
      if (!camera.quick) { query(a, b, true, covered); }
      if (need_scene_sides && !camera.quick) {
        query(a + normal, b + normal, false, left);
        query(a - normal, b - normal, false, right);
      }
      if (need_object_sides && !camera.quick) {
        /* A projected part of this object behind another object is not its visible
         * interior. Match the side ray's nearest visible owner, not any-depth coverage. */
        query_visible_object(a + normal, b + normal, object_left);
        query_visible_object(a - normal, b - normal, object_right);
      }
      bool refine_source = false;
      for (int face : faces) {
        refine_source |= geometry.surfaces[geometry.faces[face].surface].mask != nullptr;
      }
      if (refine_source && source_masks && !loose && !camera.quick) {
        auto face_present = [&](int face, double3 point) {
          const Surface &surface = geometry.surfaces[geometry.faces[face].surface];
          if (!surface.mask || surface.mask->minimum >= surface.mask->threshold) { return true; }
          if (surface.mask->maximum < surface.mask->threshold) { return false; }
          for (int j = face_starts[face]; j < face_starts[face + 1]; j++) {
            const Triangle &tri = geometry.triangles[face_triangles[j]];
            const double3 p0 = geometry.positions[tri.vertices.x];
            const double3 u = geometry.positions[tri.vertices.y] - p0;
            const double3 v = geometry.positions[tri.vertices.z] - p0;
            const double3 w = point - p0;
            const double uu = math::dot(u, u), uv = math::dot(u, v), vv = math::dot(v, v);
            const double wu = math::dot(w, u), wv = math::dot(w, v), den = uu * vv - uv * uv;
            if (std::abs(den) < 1e-28) { continue; }
            const double x = (wu * vv - wv * uv) / den, y = (wv * uu - wu * uv) / den;
            if (x >= -1e-8 && y >= -1e-8 && x + y <= 1 + 1e-8) {
              result.statistics.mask_tests++;
              return surface.mask->sample(tri.uv[0] * (1 - x - y) + tri.uv[1] * x + tri.uv[2] * y);
            }
          }
          return true; /* Degenerate faces must not silently discard a marked edge. */
        };
        const int steps = std::max(1, int(std::ceil(length * 4)));
        double run = -1;
        for (int k = 0; k < steps; k++) {
          if (cancel.load(std::memory_order_relaxed)) { return; }
          const double x = double(k) / steps, y = double(k + 1) / steps;
          const double t = world_parameter((x + y) * 0.5);
          const double3 point = world_a + (world_b - world_a) * t;
          bool present = is_intersection;
          for (int face : faces) {
            if (is_intersection) { present &= face_present(face, point); }
            else { present |= face_present(face, point); }
          }
          if (present) {
            if (run < 0) { run = x; }
            if (k + 1 == steps) { source_coverage.push_back({run, y}); }
          }
          else if (run >= 0) {
            source_coverage.push_back({run, x});
            run = -1;
          }
        }
      }
      else { source_coverage.push_back({0, 1}); }
      /* The reference samples one terminal cell in addition to floor(length * 1.5).
       * Keeping that cell avoids a phase/step mismatch at integer projected lengths. */
      const double cells = std::max(1.0, std::floor(double(float(length) * 1.5f)) + 1.0);
      for (auto *intervals : {&covered, &left, &right, &object_left, &object_right, &source_coverage}) {
        for (Interval &interval : *intervals) {
          interval.a = std::clamp(std::ceil(interval.a * cells - 0.5) / cells, 0.0, 1.0);
          interval.b = std::clamp((std::floor(interval.b * cells - 0.5) + 1) / cells, 0.0, 1.0);
        }
        std::erase_if(*intervals, [](const Interval &interval) { return interval.b <= interval.a; });
        merge_intervals(*intervals);
      }
      boundaries = {0, 1};
      for (const auto *intervals : {&covered, &left, &right, &object_left, &object_right, &source_coverage}) {
        for (const Interval interval : *intervals) {
          boundaries.push_back(interval.a);
          boundaries.push_back(interval.b);
        }
      }
      std::sort(boundaries.begin(), boundaries.end());
      boundaries.erase(std::unique(boundaries.begin(), boundaries.end(),
                                    [](double x, double y) { return std::abs(x - y) < 1e-10; }), boundaries.end());
      const Surface &surface = geometry.surfaces[geometry.faces[faces.front()].surface];
      for (int ri : selected) {
        const Rule &rule = rules[ri];
        if (is_intersection && !rule.self_intersection && faces.size() == 2 &&
            surface.instance == geometry.surfaces[geometry.faces[faces[1]].surface].instance) {
          continue;
        }
        int attribute_face = faces.front();
        for (const int face : faces) {
          const auto &claims = geometry.surfaces[geometry.faces[face].surface].rules;
          if (std::find(claims.begin(), claims.end(), ri) != claims.end()) { attribute_face = face; break; }
        }
        uint32_t candidate_types = types;
        const double radians = 3.141592653589793 / 180;
        if (!is_intersection && !contour && faces.size() > 1 &&
            normal_dot <= std::cos(rule.normal_min * radians) + 1e-10 &&
            normal_dot >= std::cos(rule.normal_max * radians) - 1e-10) {
          candidate_types |= Normal;
        }
        for (int i = 0; i + 1 < int(boundaries.size()); i++) {
          const double x = boundaries[i], y = boundaries[i + 1], mid = (x + y) * 0.5;
          if (!contains(source_coverage, mid)) { continue; }
          const bool hidden = contains(covered, mid) || (is_intersection && back);
          if (hidden && !rule.hidden_enabled) {
            continue;
          }
          const auto &left_coverage = rule.object_contours ? object_left : left;
          const auto &right_coverage = rule.object_contours ? object_right : right;
          const bool interior = camera.quick ? !(rule.visible & Outline) :
                                contains(left_coverage, mid) && contains(right_coverage, mid);
          uint32_t enabled_types = hidden ? rule.hidden : rule.visible;
        if (open && !rule.open_edges) { enabled_types &= ~(Outline | ObjectContour); }
          if (open) {
            if (!(hidden ? rule.hidden_open_outline : rule.visible_open_outline)) { enabled_types &= ~Outline; }
            if (!(hidden ? rule.hidden_open_object : rule.visible_open_object)) { enabled_types &= ~ObjectContour; }
          }
        const int type = choose_type(enabled_types, candidate_types, contour, interior, rule.contour_priority);
          if (type < 0) {
            continue;
          }
          const int brush = hidden ? rule.hidden_brush[type] : rule.visible_brush[type];
          if (brush < 0) { continue; } /* A disconnected brush is an empty style, never an index. */
          Segment segment;
          segment.a = a + (b - a) * x;
          segment.b = a + (b - a) * y;
          segment.source_start = world_parameter(x);
          segment.source_end = world_parameter(y);
          segment.attribute_a = attribute_point(attribute_face, world_a + (world_b - world_a) * segment.source_start,
                                                 (1 - x) / ca.w + x / cb.w);
          segment.attribute_b = attribute_point(attribute_face, world_a + (world_b - world_a) * segment.source_end,
                                                 (1 - y) / ca.w + y / cb.w);
          if (loose && source_edge >= 0) {
            segment.attribute_a.triangle = segment.attribute_b.triangle = -source_edge - 2;
            segment.attribute_a.barycentric = float2(float(segment.source_start), 0);
            segment.attribute_b.barycentric = float2(float(segment.source_end), 0);
          }
          segment.join_depth_a = math::dot(world_a + (world_b - world_a) * segment.source_start - camera.position, camera.forward);
          segment.join_depth_b = math::dot(world_a + (world_b - world_a) * segment.source_end - camera.position, camera.forward);
          const double3 wa = previous_world_a + (previous_world_b - previous_world_a) * segment.source_start;
          const double3 wb = previous_world_a + (previous_world_b - previous_world_a) * segment.source_end;
          const double4 pa = homogeneous(camera.previous_projection, wa);
          const double4 pb = homogeneous(camera.previous_projection, wb);
          segment.previous_a = std::abs(pa.w) > 1e-10 ? screen_position(pa, camera) : segment.a;
          segment.previous_b = std::abs(pb.w) > 1e-10 ? screen_position(pb, camera) : segment.b;
          const double4 na = homogeneous(camera.next_projection, next_world_a + (next_world_b - next_world_a) * segment.source_start);
          const double4 nb = homogeneous(camera.next_projection, next_world_a + (next_world_b - next_world_a) * segment.source_end);
          segment.next_a = std::abs(na.w) > 1e-10 ? screen_position(na, camera) : segment.a;
          segment.next_b = std::abs(nb.w) > 1e-10 ? screen_position(nb, camera) : segment.b;
          segment.identity = identity;
          segment.instance = surface.instance;
          if (is_intersection) {
            for (const int face : faces) {
              const uint64_t owner = geometry.surfaces[geometry.faces[face].surface].instance;
              if (owner != segment.instance) { segment.secondary_instance = owner;break; }
            }
          }
          segment.vertex_a = segment.source_start < 1e-10 ?
                                 (rule.repair_seams && seam_vertex_a ? seam_vertex_a : vertex_a) : 0;
          segment.vertex_b = segment.source_end > 1 - 1e-10 ?
                                 (rule.repair_seams && seam_vertex_b ? seam_vertex_b : vertex_b) : 0;
          segment.rule = ri;
          segment.brush = brush;
          segment.type = type;
          segment.width_scale = rule.width_scale[type];
          segment.hidden = hidden;
          segment.intersection = is_intersection;
          /* Different triangle-intersection pieces can share a geometric identity but
           * have different UV gradients. Connect their paths later; do not erase the
           * attribute boundary when merging visibility intervals. */
          if (result.segments.size() > process_segment_begin) {
            Segment &previous = result.segments.back();
            if (previous.identity == identity && previous.rule == ri && previous.brush == brush &&
                previous.hidden == hidden && previous.type == type &&
                math::length_squared(previous.b - segment.a) < 1e-18) {
              previous.b = segment.b;
              previous.previous_b = segment.previous_b;
              previous.next_b = segment.next_b;
              previous.join_depth_b = segment.join_depth_b;
              previous.source_end = segment.source_end;
              previous.attribute_b = segment.attribute_b;
              previous.vertex_b = segment.vertex_b;
              continue;
            }
          }
          result.segments.push_back(segment);
        }
      }
    };

    const int64_t edge_count = int64_t(geometry.edges.size());
    for (int64_t index = range.start(); index < std::min(range.one_after_last(), edge_count); index++) {
      const Edge &edge = geometry.edges[index];
      if (cancel.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return;
      }
      if (edge.face_count == 0) {
        continue;
      }
      const std::span<const int> faces(geometry.edge_faces.data() + edge.face_start, edge.face_count);
      const uint64_t instance = geometry.surfaces[geometry.faces[faces.front()].surface].instance;
      const bool has_vertex_ids = geometry.vertex_identities.size() == geometry.positions.size();
      const auto &previous = (camera.use_previous_geometry || geometry.has_motion_steps) && geometry.previous_positions.size() == geometry.positions.size() ?
                                 geometry.previous_positions : geometry.positions;
      const auto &next = geometry.has_motion_steps && geometry.next_positions.size() == geometry.positions.size() ?
                            geometry.next_positions : geometry.positions;
      const bool seam_ids = geometry.seam_vertex_identities.size() == geometry.positions.size();
      const uint64_t seam_a = seam_ids ? geometry.seam_vertex_identities[edge.vertices.x] : 0;
      const uint64_t seam_b = seam_ids ? geometry.seam_vertex_identities[edge.vertices.y] : 0;
      process(geometry.positions[edge.vertices[0]], geometry.positions[edge.vertices[1]], faces,
               edge.types, edge.normal_dot, edge.identity,
               has_vertex_ids ? geometry.vertex_identities[edge.vertices[0]] : mix_id(instance, uint64_t(edge.vertices[0]) + 1),
               has_vertex_ids ? geometry.vertex_identities[edge.vertices[1]] : mix_id(instance, uint64_t(edge.vertices[1]) + 1),
               false, edge.loose, previous[edge.vertices.x], previous[edge.vertices.y],
               next[edge.vertices.x], next[edge.vertices.y], edge.seam_face >= 0 ? 1 : 0, seam_a, seam_b, int(index));
      if (edge.seam_face >= 0 && !edge.seam_secondary) {
        const std::array<int, 2> joined_faces{faces.front(), edge.seam_face};
        const double normal_dot = math::dot(geometry.faces[joined_faces[0]].normal,
                                            geometry.faces[joined_faces[1]].normal);
        process(geometry.positions[edge.vertices.x], geometry.positions[edge.vertices.y], joined_faces,
                 edge.seam_types, normal_dot, edge.identity,
                 has_vertex_ids ? geometry.vertex_identities[edge.vertices.x] : 0,
                 has_vertex_ids ? geometry.vertex_identities[edge.vertices.y] : 0,
                 false, false, previous[edge.vertices.x], previous[edge.vertices.y],
                 next[edge.vertices.x], next[edge.vertices.y], 2, seam_a, seam_b);
      }
    }
    const int64_t intersection_begin = std::max<int64_t>(0, range.start() - edge_count);
    const int64_t intersection_end = std::max<int64_t>(0, range.one_after_last() - edge_count);
    for (int64_t index = intersection_begin; index < intersection_end; index++) {
      const IntersectionEdge &edge = intersections[index];
      if (cancel.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return;
      }
      const std::array<int, 2> faces = {edge.faces.x, edge.faces.y};
      auto step_intersection = [&](const std::vector<double3> &positions) {
        std::array<double3, 2> line = {edge.a, edge.b};
        if (positions.size() != geometry.positions.size()) { return line; }
        const Triangle &a = geometry.triangles[edge.triangles.x];
        const Triangle &b = geometry.triangles[edge.triangles.y];
        double3 from, to;
        if (intersect_triangles({positions[a.vertices.x], positions[a.vertices.y], positions[a.vertices.z]},
                                {positions[b.vertices.x], positions[b.vertices.y], positions[b.vertices.z]}, from, to)) {
          if (math::length_squared(from - edge.a) + math::length_squared(to - edge.b) >
              math::length_squared(to - edge.a) + math::length_squared(from - edge.b)) { std::swap(from, to); }
          line = {from, to};
        }
        return line;
      };
      const auto previous = camera.use_previous_geometry || geometry.has_motion_steps ?
                                step_intersection(geometry.previous_positions) : std::array<double3, 2>{edge.a, edge.b};
      const auto next = geometry.has_motion_steps ? step_intersection(geometry.next_positions) :
                                                    std::array<double3, 2>{edge.a, edge.b};
      process(edge.a, edge.b, faces, Intersection, 1, edge.identity, 0, 0, true, false,
               previous[0], previous[1], next[0], next[1]);
    }
  };
  const int64_t work_count = int64_t(geometry.edges.size()) + int64_t(intersections.size());
  if (work_count < 4096) {
    process_range(IndexRange(work_count), result);
  }
  else {
    constexpr int64_t batch_size = 1024;
    const int64_t batch_count = (work_count + batch_size - 1) / batch_size;
    std::vector<Result> batches(batch_count);
    threading::parallel_for(IndexRange(batch_count), 1, [&](IndexRange batch_range) {
      for (const int64_t batch : batch_range) {
        const int64_t begin = batch * batch_size;
        process_range(IndexRange(begin, std::min(batch_size, work_count - begin)), batches[batch]);
      }
    });
    size_t count = 0;
    for (const Result &batch : batches) {
      if (batch.cancelled || cancel.load(std::memory_order_relaxed)) {
        result.cancelled = true;
        return result;
      }
      count += batch.segments.size();
    }
    result.segments.reserve(count);
    for (Result &batch : batches) {
      result.statistics.candidates += batch.statistics.candidates;
      result.statistics.triangle_tests += batch.statistics.triangle_tests;
      result.statistics.mask_tests += batch.statistics.mask_tests;
      result.segments.insert(result.segments.end(), batch.segments.begin(), batch.segments.end());
      std::vector<Segment>().swap(batch.segments);
    }
  }
  result.statistics.visibility_ms = elapsed(start);
  if (cancel.load(std::memory_order_relaxed)) { result.cancelled = true; return result; }
  if (connect) { connect_strokes(result, rules, cancel); }
  return result;
}

}  // namespace blender::eevee::geometry_lines
