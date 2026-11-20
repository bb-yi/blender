/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "eevee_geometry_lines_geometry.hh"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

#include "BLI_math_vector.hh"

namespace blender::eevee::geometry_lines {
namespace {

uint64_t hash_float(float value)
{
  if (value == 0) {
    value = 0; /* Canonicalize negative zero. */
  }
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  uint64_t hash = 14695981039346656037ULL;
  for (int i = 0; i < 4; i++) {
    hash = (hash ^ ((bits >> (8 * i)) & 255)) * 1099511628211ULL;
  }
  return hash;
}

uint16_t endpoint_hash(float3 p)
{
  uint64_t hash = hash_float(p.z);
  hash = ((hash << 1) | (hash >> 63)) ^ hash_float(p.x);
  hash = ((hash << 1) | (hash >> 63)) ^ hash_float(p.y);
  return uint16_t(hash);
}

void reverse_segment(Segment &segment)
{
  std::swap(segment.a, segment.b);
  std::swap(segment.join_depth_a, segment.join_depth_b);
  std::swap(segment.previous_a, segment.previous_b);
  std::swap(segment.next_a, segment.next_b);
  std::swap(segment.vertex_a, segment.vertex_b);
  std::swap(segment.source_start, segment.source_end);
  std::swap(segment.attribute_a, segment.attribute_b);
}

bool orient(Stroke &stroke, const Rule &rule, const std::atomic<bool> &cancel)
{
  const double angle = rule.direction * (3.141592653589793 / 180);
  const double2 preferred(std::cos(angle), std::sin(angle));
  auto reverse = [&]() -> bool {
    std::reverse(stroke.parts.begin(), stroke.parts.end());
    for (int i = 0; i < int(stroke.parts.size()); i++) {
      if ((i & 1023) == 0 && cancel.load(std::memory_order_relaxed)) { return false; }
      reverse_segment(stroke.parts[i].segment);
      std::swap(stroke.parts[i].offset_normal_a, stroke.parts[i].offset_normal_b);
    }
    return true;
  };
  if (!stroke.closed) {
    double score = 0;
    for (int i = 0; i < int(stroke.parts.size()); i++) {
      if ((i & 1023) == 0 && cancel.load(std::memory_order_relaxed)) { return false; }
      const auto &part = stroke.parts[i];
      const double3 d = part.segment.b - part.segment.a;
      score += d.x * preferred.x + d.y * preferred.y;
    }
    if (score < 0) {
      if (!reverse()) { return false; }
    }
    return true;
  }
  double area = 0;
  double2 lower(std::numeric_limits<double>::max());
  double2 upper(-std::numeric_limits<double>::max());
  for (int i = 0; i < int(stroke.parts.size()); i++) {
    if ((i & 1023) == 0 && cancel.load(std::memory_order_relaxed)) { return false; }
    const auto &part = stroke.parts[i];
    area += part.segment.a.x * part.segment.b.y - part.segment.b.x * part.segment.a.y;
    for (const double3 &p : {part.segment.a, part.segment.b}) {
      lower.x = std::min(lower.x, p.x);lower.y = std::min(lower.y, p.y);
      upper.x = std::max(upper.x, p.x);upper.y = std::max(upper.y, p.y);
    }
  }
  /* Choose the anchor edge in canonical clockwise order. Reversing a loop then
   * starts at the other endpoint of that same edge, preserving its identity. */
  if (area >= 0) {
    if (!reverse()) { return false; }
  }
  const double2 center = (lower + upper) * 0.5;
  const double2 weighted((upper.x - lower.x) * preferred.x,
                          (upper.y - lower.y) * preferred.y);
  const double length = std::sqrt(weighted.x * weighted.x + weighted.y * weighted.y);
  const double2 anchor_direction = length > 0 ? weighted / length : double2(1, 0);
  int first = 0;
  double best = std::numeric_limits<double>::max();
  for (int i = 0; i < int(stroke.parts.size()); i++) {
    if ((i & 1023) == 0 && cancel.load(std::memory_order_relaxed)) { return false; }
    const auto &segment = stroke.parts[i].segment;
    const double2 p((segment.a.x + segment.b.x) * 0.5 - center.x,
                    (segment.a.y + segment.b.y) * 0.5 - center.y);
    const double score = p.x * anchor_direction.x + p.y * anchor_direction.y;
    if (score < best) {
      best = score;
      first = i;
    }
  }
  std::rotate(stroke.parts.begin(), stroke.parts.begin() + first, stroke.parts.end());
  if (!rule.clockwise) {
    if (!reverse()) { return false; }
    std::rotate(stroke.parts.begin(), stroke.parts.end() - 1, stroke.parts.end());
  }
  return !cancel.load(std::memory_order_relaxed);
}

void connect_rule(Result &result,
                   const std::vector<int> &segment_indices,
                   const Rule &rule,
                   const std::atomic<bool> &cancel)
{
  const int segment_count = int(segment_indices.size()), endpoint_count = segment_count * 2;
  if (!endpoint_count) {
    return;
  }
  std::vector<float3> positions(endpoint_count);
  std::vector<float2> directions(endpoint_count);
  std::vector<float> segment_lengths(segment_count);
  std::vector<int> links(endpoint_count, -1), best(endpoint_count, -1), indices(endpoint_count);
  std::vector<double> scores(endpoint_count);
  std::vector<uint16_t> keys(endpoint_count), occupied(65536);
  std::vector<int> counts(65536), starts(65536), cursor(65536);
  struct Block {
    float3 lower, upper;
    int begin, end;
  };
  std::vector<Block> blocks;
  std::vector<int> block_starts(65536, -1), block_counts(65536);
  auto cancelled = [&]() {
    if (!cancel.load(std::memory_order_relaxed)) {
      return false;
    }
    result.cancelled = true;
    return true;
  };
  auto segment = [&](int endpoint) -> const Segment & {
    return result.segments[segment_indices[endpoint >> 1]];
  };
  for (int i = 0; i < segment_count; i++) {
    if ((i & 1023) == 0 && cancelled()) { return; }
    const Segment &s = segment(i * 2);
    positions[i * 2] = float3(s.a);
    positions[i * 2 + 1] = float3(s.b);
    positions[i * 2].z = float(s.join_depth_a);
    positions[i * 2 + 1].z = float(s.join_depth_b);
    const double2 d(s.b.x - s.a.x, s.b.y - s.a.y);
    const double length = math::length(d);
    segment_lengths[i] = float(length);
    directions[i * 2] = float2(d / std::max(length, 1e-20));
    directions[i * 2 + 1] = -directions[i * 2];
  }
  std::vector<float2> cap_directions(directions);
  std::vector<int> gap_endpoints, path_ids(endpoint_count, -1);
  std::vector<float> path_lengths;
  const double negative = -std::numeric_limits<double>::infinity();
  auto match = [&](int kind, double radius) -> bool {
    const bool exact = kind == 0;
    if (kind == 2) { radius = std::max(radius, 0.001); }
    const float far_weight = kind == 2 ? std::sqrt(std::pow(1.7071067f, 4.0f)) /
                                           (float(radius) * float(radius)) : 0.0f;
    const double depth_limit = kind == 2 && rule.screen_space_join ?
                                   std::numeric_limits<double>::infinity() : rule.join_depth;
    int cells = 0;
    std::fill(counts.begin(), counts.end(), 0);
    const double cw = std::max(result.extent.x / 256.0, radius);
    const double ch = std::max(result.extent.y / 256.0, radius);
    auto eligible = [&](int i) {
      return links[i] < 0 && (kind != 0 || !segment(i).intersection) &&
             (kind != 1 || segment(i).intersection);
    };
    const int ordered_count = kind == 2 ? int(gap_endpoints.size()) : endpoint_count;
    for (int k = 0; k < ordered_count; k++) {
      const int i = kind == 2 ? gap_endpoints[k] : k;
      if ((i & 1023) == 0 && cancelled()) { return false; }
      if (!eligible(i)) {
        continue;
      }
      const float3 p = positions[i];
      const int key = exact ? endpoint_hash(p) :
                             std::clamp(int(std::floor(p.y / ch)), 0, 255) * 256 +
                                 std::clamp(int(std::floor(p.x / cw)), 0, 255);
      keys[i] = uint16_t(key);
      if (counts[key]++ == 0) {
        occupied[cells++] = uint16_t(key);
      }
      best[i] = -1;
      scores[i] = negative;
    }
    int offset = 0;
    for (int k = 0; k < cells; k++) {
      const int key = occupied[k];
      starts[key] = cursor[key] = offset;
      offset += counts[key];
    }
    for (int k = 0; k < ordered_count; k++) {
      const int i = kind == 2 ? gap_endpoints[k] : k;
      if ((i & 1023) == 0 && cancelled()) { return false; }
      if (eligible(i)) {
        indices[cursor[keys[i]]++] = i;
      }
    }
    auto compare = [&](int a, int b) {
      result.statistics.endpoint_tests++;
      if ((a >> 1) == (b >> 1)) {
        return;
      }
      const Segment &sa = segment(a), &sb = segment(b);
      const bool shared_owner = sa.instance == sb.instance ||
          (kind == 2 && ((sa.secondary_instance && (sa.secondary_instance == sb.instance ||
                                                    sa.secondary_instance == sb.secondary_instance)) ||
                        (sb.secondary_instance && sb.secondary_instance == sa.instance)));
      if (sa.hidden != sb.hidden || sa.brush != sb.brush ||
          (!rule.connect_objects && !shared_owner)) {
        return;
      }
      const float3 d = positions[a] - positions[b];
      if (exact) {
        if (d.x != 0 || d.y != 0 || d.z != 0) {
          return;
        }
        const uint64_t va = a & 1 ? sa.vertex_b : sa.vertex_a;
        const uint64_t vb = b & 1 ? sb.vertex_b : sb.vertex_a;
        /* Preserve original mesh topology; coincident disconnected pieces do not weld. */
        if (!rule.connect_objects && !rule.connect_coincident &&
            !((va && va == vb) || sa.identity == sb.identity)) {
          return;
        }
      }
      else if (double(d.x) * d.x + double(d.y) * d.y > radius * radius || std::abs(d.z) > depth_limit) {
        return;
      }
      const float2 da = kind == 2 ? cap_directions[a] : directions[a];
      const float2 db = kind == 2 ? cap_directions[b] : directions[b];
      if (kind == 2 && path_ids[a] >= 0 && path_ids[a] == path_ids[b]) {
        const float tolerance = path_lengths[path_ids[a]] * 0.25f;
        volatile float ax = d.x * da.x, ay = d.y * da.y;
        volatile float bx = d.x * db.x, by = d.y * db.y;
        if (std::abs(float(ax + ay)) > tolerance || std::abs(float(bx + by)) > tolerance) { return; }
      }
      /* Match the reference's explicitly rounded float32 products, including ties. */
      volatile float x = -da.x * db.x;
      volatile float y = -da.y * db.y;
      const float dot = x + y;
      double score;
      if (exact) {
        score = float(dot + (sa.type == sb.type ? 2.0f : 0.0f));
      }
      else if (kind == 2) {
        const float angle = 1.0f + dot;
        const float angle_squared = angle * angle;
        const float angular = angle_squared * angle_squared;
        volatile float dx2 = d.x * d.x, dy2 = d.y * d.y;
        const float upper = angular - std::max(float(dx2), float(dy2)) * far_weight;
        if (upper <= scores[a] && upper <= scores[b]) { return; }
        const float distance_squared = dx2 + dy2;
        score = angular - distance_squared * far_weight;
      }
      else {
        const double base = double(dot) + (sa.type == sb.type ? 0.3 : 0);
        const double upper = base - std::max(std::abs(d.x), std::abs(d.y)) / std::max(radius, 0.001) * 0.2 + 1e-12;
        if (upper <= scores[a] && upper <= scores[b]) {
          return;
        }
        score = base - std::hypot(double(d.x), double(d.y)) / std::max(radius, 0.001) * 0.2;
      }
      if (score > scores[a]) {
        scores[a] = score;
        best[a] = b;
      }
      if (score > scores[b]) {
        scores[b] = score;
        best[b] = a;
      }
    };
    constexpr int neighbors[][2] = {{1, 0}, {0, 1}, {1, 1}, {-1, 1}};
    while (cells) {
      if (cancelled()) { return false; }
      blocks.clear();
      for (int k = 0; k < cells; k++) {
        if ((k & 255) == 0 && cancelled()) { return false; }
        const int key = occupied[k], begin = starts[key], end = begin + counts[key];
        block_starts[key] = -1;
        block_counts[key] = 0;
        /* Small buckets already do little work. Larger buckets keep their exact endpoint
         * order while each consecutive group of 16 gets a conservative geometric bound. */
        if (counts[key] < 32) { continue; }
        block_starts[key] = int(blocks.size());
        for (int j = begin; j < end; j += 16) {
          if (((j - begin) & 1023) == 0 && cancelled()) { return false; }
          Block block{positions[indices[j]], positions[indices[j]], j, std::min(j + 16, end)};
          for (int index = j + 1; index < block.end; index++) {
            const float3 p = positions[indices[index]];
            block.lower = math::min(block.lower, p);
            block.upper = math::max(block.upper, p);
          }
          blocks.push_back(block);
          block_counts[key]++;
        }
      }
      auto visit = [&](int a, int key, int begin) -> bool {
        const int end = starts[key] + counts[key];
        if (block_starts[key] < 0) {
          for (int b = begin; b < end; b++) { compare(a, indices[b]); }
          return true;
        }
        const int first = block_starts[key] + (begin - starts[key]) / 16;
        const int last = block_starts[key] + block_counts[key];
        for (int i = first; i < last; i++) {
          if (cancelled()) { return false; }
          const Block &block = blocks[i];
          const float3 p = positions[a];
          /* Compute lower bounds with the same float subtraction as compare(), then promote
           * for squaring. A double-precision bound could incorrectly exclude a float-rounded
           * pair lying exactly on the radius/depth threshold. */
          const float3 lower = math::max(math::max(block.lower - p, p - block.upper), float3(0));
          if (exact ? (lower.x != 0 || lower.y != 0 || lower.z != 0) :
                      (double(lower.x) * lower.x + double(lower.y) * lower.y > radius * radius ||
                       lower.z > depth_limit)) {
            continue;
          }
          for (int b = std::max(begin, block.begin); b < block.end; b++) {
            compare(a, indices[b]);
          }
        }
        return true;
      };
      for (int k = 0; k < cells; k++) {
        if (cancelled()) { return false; }
        const int key = occupied[k], begin = starts[key], end = begin + counts[key];
        for (int a = begin; a < end; a++) {
          if (cancelled() || !visit(indices[a], key, a + 1)) { return false; }
        }
        if (!exact) {
          const int x = key & 255, y = key >> 8;
          for (const auto &neighbor : neighbors) {
            const int xx = x + neighbor[0], yy = y + neighbor[1];
            if (xx < 0 || xx > 255 || yy < 0 || yy > 255) {
              continue;
            }
            const int other = yy * 256 + xx;
            if (counts[other] == 0) { continue; }
            for (int a = begin; a < end; a++) {
              if (cancelled() || !visit(indices[a], other, starts[other])) { return false; }
            }
          }
        }
      }
      int matched = 0;
      for (int k = 0; k < cells; k++) {
        if ((k & 255) == 0 && cancelled()) { return false; }
        const int key = occupied[k], end = starts[key] + counts[key];
        for (int j = starts[key]; j < end; j++) {
          if (((j - starts[key]) & 1023) == 0 && cancelled()) { return false; }
          const int a = indices[j], b = best[a];
          if (b > a && best[b] == a) {
            links[a] = b;
            links[b] = a;
            matched++;
          }
        }
      }
      /* Keep the reference's swap-removal order for subsequent matching rounds. */
      for (int k = 0; k < cells;) {
        if ((k & 255) == 0 && cancelled()) { return false; }
        const int key = occupied[k], begin = starts[key];
        for (int j = 0; j < counts[key];) {
          if ((j & 1023) == 0 && cancelled()) { return false; }
          const int a = indices[begin + j];
          if (links[a] >= 0 || best[a] < 0) {
            indices[begin + j] = indices[begin + --counts[key]];
          }
          else {
            j++;
          }
        }
        if (!counts[key]) {
          occupied[k] = occupied[--cells];
        }
        else {
          k++;
        }
      }
      if (!matched) {
        break;
      }
      for (int k = 0; k < cells; k++) {
        const int key = occupied[k];
        for (int j = starts[key]; j < starts[key] + counts[key]; j++) {
          if (((j - starts[key]) & 1023) == 0 && cancelled()) { return false; }
          best[indices[j]] = -1;
          scores[indices[j]] = negative;
        }
      }
    }
    return true;
  };
  auto split = [&](bool intersection_only = false) -> bool {
    /* Measure turns on a linked path, not on an almost edge-on individual edge.
     * The verified reference uses a capped third of the available path length,
     * then recursively splits its sharpest eligible turn with protected ends. */
    constexpr float end_guard = 3.0f;
    constexpr float tangent_fraction = 0.3333f;
    auto tangent = [&](int endpoint, float window) {
      const float2 origin = positions[endpoint].xy();
      if (window < segment_lengths[endpoint >> 1]) {
        return directions[endpoint];
      }
      float2 target = origin;
      float distance = 0;
      int current = endpoint;
      for (int step = 0; step < segment_count; step++) {
        if ((step & 1023) == 0 && cancel.load(std::memory_order_relaxed)) { break; }
        const float2 a = positions[current].xy(), b = positions[current ^ 1].xy();
        const float length = segment_lengths[current >> 1];
        if (length > 0 && distance + length >= window) {
          target = a + (b - a) * ((window - distance) / length);
          break;
        }
        target = b;
        distance += length;
        const int next = links[current ^ 1];
        if (next < 0 || next == endpoint) { break; }
        current = next;
      }
      const float2 delta = target - origin;
      const float length = math::length(delta);
      return length > 0 ? delta / length : float2(1, 0);
    };
    const float limit = std::cos(rule.split_angle * float(3.141592653589793 / 180));
    std::vector<uint8_t> visited(segment_count, 0);
    std::vector<int> path;
    auto split_path = [&](bool closed) -> bool {
      const int count = int(path.size());
      float total = 0;
      for (int endpoint : path) { total += segment_lengths[endpoint >> 1]; }
      auto set_end_caps = [&]() {
        const float window = std::min(total * tangent_fraction, end_guard);
        cap_directions[path.front()] = tangent(path.front(), window);
        cap_directions[path.back() ^ 1] = tangent(path.back() ^ 1, window);
      };
      if (!closed) { set_end_caps(); }
      if (count < 2 || total < end_guard) { return true; }
      auto sever = [&](int index) {
        const int a = path[(index + count - 1) % count] ^ 1, b = path[index];
        links[a] = links[b] = -1;
      };
      if (closed) {
        const float window = std::min(total * tangent_fraction, end_guard);
        float worst = limit;
        int cut = -1;
        for (int j = 1; j <= count; j++) {
          if ((j & 1023) == 0 && cancelled()) { return false; }
          const int i = j % count;
          const float score = -math::dot(tangent(path[j - 1] ^ 1, window), tangent(path[i], window));
          if (score < worst) { worst = score; cut = i; }
        }
        if (cut < 0) { return true; }
        sever(cut);
        std::rotate(path.begin(), path.begin() + cut, path.end());
        set_end_caps();
      }
      std::vector<float> prefix(count + 1, 0), turns(count, 1.0f);
      std::vector<float2> turn_before(count), turn_after(count);
      for (int i = 0; i < count; i++) { prefix[i + 1] = prefix[i] + segment_lengths[path[i] >> 1]; }
      for (int i = 1; i < count; i++) {
        if ((i & 1023) == 0 && cancelled()) { return false; }
        const float left = std::min(prefix[i] * tangent_fraction, end_guard);
        const float right = std::min((total - prefix[i]) * tangent_fraction, end_guard);
        turn_before[i] = tangent(path[i - 1] ^ 1, left);
        turn_after[i] = tangent(path[i], right);
        turns[i] = -math::dot(turn_before[i], turn_after[i]);
      }
      auto better = [&](int a, int b) {
        if (a < 0) { return b; }
        if (b < 0) { return a; }
        return turns[a] < turns[b] || (turns[a] == turns[b] && a < b) ? a : b;
      };
      /* Cached turn values allow range minima without quadratic rescans of a long
       * zigzag. Equal scores still select the first junction in path order. */
      int leaves = 0;
      std::vector<int> minima;
      if (count >= 128) {
        leaves = 1;
        while (leaves < count) { leaves *= 2; }
        minima.assign(leaves * 2, -1);
        for (int i = 1; i < count; i++) { minima[leaves + i] = i; }
        for (int i = leaves - 1; i; i--) { minima[i] = better(minima[i * 2], minima[i * 2 + 1]); }
      }
      std::vector<int2> pending{{0, count}};
      while (!pending.empty()) {
        if (cancelled()) { return false; }
        const int2 range = pending.back();
        pending.pop_back();
        int begin = int(std::lower_bound(prefix.begin() + range.x + 1, prefix.begin() + range.y,
                                         prefix[range.x] + end_guard) - prefix.begin());
        int end = int(std::upper_bound(prefix.begin() + begin, prefix.begin() + range.y,
                                       prefix[range.y] - end_guard) - prefix.begin());
        if (begin >= end) { continue; }
        int cut = -1;
        if (leaves) {
          for (begin += leaves, end += leaves; begin < end; begin /= 2, end /= 2) {
            if (begin & 1) { cut = better(cut, minima[begin++]); }
            if (end & 1) { cut = better(cut, minima[--end]); }
          }
        }
        else {
          for (int i = begin; i < end; i++) { cut = better(cut, i); }
        }
        if (cut >= 0 && turns[cut] < limit) {
          cap_directions[path[cut - 1] ^ 1] = turn_before[cut];
          cap_directions[path[cut]] = turn_after[cut];
          sever(cut);
          pending.push_back({cut, range.y});
          pending.push_back({range.x, cut});
        }
      }
      return true;
    };
    auto visit = [&](int start) -> bool {
      path.clear();
      int current = start;
      while (current >= 0 && !visited[current >> 1]) {
        if ((path.size() & 1023) == 0 && cancelled()) { return false; }
        visited[current >> 1] = 1;
        path.push_back(current);
        current = links[current ^ 1];
      }
      return split_path(current == start);
    };
    for (int i = 0; i < endpoint_count; i++) {
      if (links[i] < 0 && !visited[i >> 1] && (!intersection_only || segment(i).intersection) && !visit(i)) { return false; }
    }
    for (int i = 0; i < segment_count; i++) {
      if (!visited[i] && (!intersection_only || segment(i * 2).intersection) && !visit(i * 2)) { return false; }
    }
    return true;
  };
  if (!match(0, 0) || !split()) { return; }
  if (rule.intersection_join > 0) {
    if (!match(1, rule.intersection_join)) { return; }
  }
  if (!split(true)) { return; }
  if (rule.gap_join > 0) {
    /* Far matching operates on open path ends, in path order. Keep these records
     * fixed through all mutual-best rounds, as in the reference's LineInfo table. */
    std::vector<uint8_t> visited(segment_count, 0);
    for (int start = 0; start < endpoint_count; start++) {
      if ((start & 1023) == 0 && cancelled()) { return; }
      if (links[start] >= 0 || visited[start >> 1]) { continue; }
      int current = start, last = start ^ 1;
      float length = 0;
      while (current >= 0 && !visited[current >> 1]) {
        if (cancelled()) { return; }
        visited[current >> 1] = 1;
        length += segment_lengths[current >> 1];
        last = current ^ 1;
        current = links[last];
      }
      const int id = int(path_lengths.size());
      path_lengths.push_back(length);
      path_ids[start] = path_ids[last] = id;
      gap_endpoints.push_back(start);
      gap_endpoints.push_back(last);
    }
    if (!match(2, rule.gap_join)) { return; }
  }
  if (!split()) { return; }
  std::vector<bool> used(segment_count, false);
  auto walk = [&](int start) {
    Stroke stroke;
    int current = start;
    for (int i = 0; i <= segment_count; i++) {
      if ((i & 255) == 0 && cancelled()) { return; }
      if (used[current >> 1]) {
        stroke.closed = current == start;
        break;
      }
      used[current >> 1] = true;
      Segment part = segment(current);
      if (current & 1) {
        reverse_segment(part);
      }
      stroke.parts.push_back({part});
      const int next = links[current ^ 1];
      if (next < 0) {
        break;
      }
      current = next;
    }
    if (stroke.parts.empty()) {
      return;
    }
    /* Offset normals belong to the connected path before start/loop direction is chosen.
     * Reversing the pen direction reverses the phase, not the side of an existing offset. */
    std::vector<float2> normals(stroke.parts.size());
    float2 previous(0, 1);
    for (const StrokePart &part : stroke.parts) {
      const double3 d = part.segment.b - part.segment.a;
      const double length = std::hypot(d.x, d.y);
      if (length > 1e-12) { previous = float2(-d.y / length, d.x / length); break; }
    }
    if (stroke.closed) {
      for (auto it = stroke.parts.rbegin(); it != stroke.parts.rend(); ++it) {
        const double3 d = it->segment.b - it->segment.a;
        const double length = std::hypot(d.x, d.y);
        if (length > 1e-12) { previous = float2(-d.y / length, d.x / length); break; }
      }
    }
    for (int i = 0; i < int(stroke.parts.size()); i++) {
      const double3 d = stroke.parts[i].segment.b - stroke.parts[i].segment.a;
      const double length = std::hypot(d.x, d.y);
      if (length > 1e-12) { previous = float2(-d.y / length, d.x / length); }
      normals[i] = previous;
    }
    auto average = [](float2 a, float2 b) {
      const float2 sum = a + b;
      const float length = std::sqrt(math::dot(sum, sum));
      return length > 1e-10f ? sum / length : b;
    };
    for (int i = 0, count = int(stroke.parts.size()); i < count; i++) {
      stroke.parts[i].offset_normal_a = i == 0 && !stroke.closed ? normals[i] :
                                           average(normals[(i + count - 1) % count], normals[i]);
      stroke.parts[i].offset_normal_b = i + 1 == count && !stroke.closed ? normals[i] :
                                           average(normals[i], normals[(i + 1) % count]);
    }
    if (!orient(stroke, rule, cancel)) {
      result.cancelled = true;
      return;
    }
    double distance = 0, painted_length = 0;
    uint64_t anchor = std::numeric_limits<uint64_t>::max();
    for (int i = 0; i < int(stroke.parts.size()); i++) {
      if ((i & 255) == 0 && cancelled()) { return; }
      StrokePart &part = stroke.parts[i];
      if (i) {
        const double3 d = part.segment.a - stroke.parts[i - 1].segment.b;
        distance += std::hypot(d.x, d.y); /* Arc continuity does not draw the gap. */
      }
      part.arc = distance;
      const double3 d = part.segment.b - part.segment.a;
      part.length = std::hypot(d.x, d.y);
      distance += part.length;
      painted_length += part.length;
      anchor = std::min(anchor, part.segment.identity);
    }
    stroke.length = distance;
    stroke.identity = anchor;
    stroke.brush = stroke.parts.front().segment.brush;
    if (painted_length >= rule.minimum_length) {
      result.strokes.push_back(std::move(stroke));
    }
  };
  for (int i = 0; i < endpoint_count; i++) {
    if ((i & 1023) == 0 && cancelled()) { return; }
    if (links[i] < 0 && !used[i >> 1]) {
      walk(i);
    }
  }
  for (int i = 0; i < segment_count; i++) {
    if ((i & 1023) == 0 && cancelled()) { return; }
    if (!used[i]) {
      walk(i * 2);
    }
  }
}

}  // namespace

void connect_strokes(Result &result,
                     const std::vector<Rule> &rules,
                     const std::atomic<bool> &cancel)
{
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::vector<int>> by_rule(rules.size());
  for (int i = 0; i < int(result.segments.size()); i++) {
    if ((i & 1023) == 0 && cancel.load(std::memory_order_relaxed)) {
      result.cancelled = true;
      return;
    }
    if (result.segments[i].brush >= 0) { by_rule[result.segments[i].rule].push_back(i); }
  }
  for (int i = 0; i < int(rules.size()); i++) {
    if (cancel.load(std::memory_order_relaxed)) {
      result.cancelled = true;
      return;
    }
    const Rule &rule = rules[i];
    if (rule.brush_connections.empty()) {
      connect_rule(result, by_rule[i], rule, cancel);
    }
    else {
      /* Different brushes never match endpoints. Partition only when needed and
       * retain the original candidate order within each independent brush. */
      std::vector<std::vector<int>> groups(rule.brush_connections.size() + 1);
      for (const int index : by_rule[i]) {
        const int brush = result.segments[index].brush;
        const auto found = std::find_if(rule.brush_connections.begin(), rule.brush_connections.end(),
                                        [&](const auto &item) { return item.first == brush; });
        const int group = found == rule.brush_connections.end() ? 0 :
                              int(found - rule.brush_connections.begin()) + 1;
        groups[group].push_back(index);
      }
      connect_rule(result, groups[0], rule, cancel);
      for (int j = 0; j < int(rule.brush_connections.size()) && !result.cancelled; j++) {
        Rule local = rule;
        static_cast<StrokeConnection &>(local) = rule.brush_connections[j].second;
        connect_rule(result, groups[j + 1], local, cancel);
      }
    }
    if (result.cancelled) { return; }
  }
  result.statistics.connection_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace blender::eevee::geometry_lines
