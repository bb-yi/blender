#include "gpu_shader_common_hash.glsl"
#include "gpu_shader_math_vector_safe_lib.glsl"

#define HRATIO 1.1547005f
#define HSQRT3 1.7320508f
#define HSQRT2 1.4142136f

#define HORIZONTAL 0
#define VERTICAL 1
#define HORIZONTAL_TILED 2
#define VERTICAL_TILED 3

float sdf_dimension(float w, float &roundness)
{
  float sw = sign(w);
  w = abs(w);
  roundness = mix(0.0f, w, clamp(roundness, 0.0f, 1.0f));
  float dim = max(w - roundness, 0.0f);
  roundness *= 0.5f;
  return dim * sw;
}

float hex_value_sdf(float3 pos, float r, float rd)
{
  float2 p = pos.xy;
  r = sdf_dimension(r, rd);
  const float3 k = float3(HSQRT3 * -0.5f, 0.5f, HRATIO * 0.5f);
  p = abs(p);
  p -= 2.0f * min(dot(k.xy, p.xy), 0.0f) * k.xy;
  p -= float2(clamp(p.x, -k.z * r, k.z * r), r);
  return length(p) * sign(p.y) - rd * 2.0f;
}

float hex_value(float3 hp, float radius)
{
  float3 fac = float3(abs(hp.x - hp.y), abs(hp.y - hp.z), abs(hp.z - hp.x));
  float f = max(fac.x, max(fac.y, fac.z));
  return (radius == 0.0f) ? f : mix(f, length(fac) / HSQRT2, radius);
}

float3 xy_to_hex(float3 xy, float ratio)
{
  float3 p = xy;
  p.x *= ratio;
  p.z = -0.5f * p.x - p.y;
  p.y = -0.5f * p.x + p.y;
  return p;
}

float hexagon_compatible_mod(float a, float b)
{
  return (b != 0.0f && a != b) ? a - b * floor(a / b) : 0.0f;
}

float hexagon(float3 p,
              float scale,
              float size,
              float radius,
              float roundness,
              int coord_mode,
              int value_mode,
              int direction,
              out float4 cell_color,
              out float3 hex_coords,
              out float3 grid_position,
              out float3 cell_coords,
              out float3 cell_id)
{
  float ratio = (direction == HORIZONTAL_TILED || direction == VERTICAL_TILED) ? 1.0f : HRATIO;
  if (direction == VERTICAL || direction == VERTICAL_TILED) {
    p = p.yxz;
  }
  p = xy_to_hex(p * scale, ratio);
  hex_coords = p;

  float3 ip = floor(p + 0.5f);
  float s = ip.x + ip.y + ip.z;
  if (s != 0.0f) {
    float3 abs_d = abs(ip - p);
    if (abs_d.x >= abs_d.y && abs_d.x >= abs_d.z) {
      ip.x -= s;
    }
    else if (abs_d.y >= abs_d.x && abs_d.y >= abs_d.z) {
      ip.y -= s;
    }
    else {
      ip.z -= s;
    }
  }

  float3 hp = p - ip;
  hp *= (size != 0.0f) ? 1.0f / size : 0.0f;
  float3 xy_coords = float3(hp.x * HSQRT3, hp.y - hp.z, 0.0f);
  if (coord_mode == 1) {
    cell_coords = hp;
    cell_id = ip;
  }
  else {
    cell_coords = xy_coords;
    cell_id = float3(
        ip.x / ratio, (ip.y - ip.z + (1.0f - hexagon_compatible_mod(ip.x, 2.0f))) / 2.0f, 0.0f);
  }
  if (direction == VERTICAL || direction == VERTICAL_TILED) {
    hp = hp.yxz;
    cell_coords = cell_coords.yxz;
    cell_id = cell_id.yxz;
  }

  grid_position = safe_divide(cell_id, float3(scale));
  cell_color = float4(hash_vec3_to_vec3(cell_id), 1.0f);

  if (value_mode == 2) {
    return length(hp);
  }
  else if (value_mode == 1) {
    return hex_value_sdf(xy_coords, radius, roundness);
  }
  else {
    return hex_value(hp, radius);
  }
}

[[node]]
void node_tex_hexagon(float3 co,
                      float scale,
                      float size,
                      float radius,
                      float roundness,
                      float coord_mode,
                      float value_mode,
                      float direction,
                      float &value,
                      float4 &cell_color,
                      float3 &coords,
                      float3 &position,
                      float3 &cell_coords,
                      float3 &cell)
{
  value = hexagon(co,
                  scale,
                  size,
                  radius,
                  roundness,
                  int(coord_mode),
                  int(value_mode),
                  int(direction),
                  cell_color,
                  coords,
                  position,
                  cell_coords,
                  cell);
}
