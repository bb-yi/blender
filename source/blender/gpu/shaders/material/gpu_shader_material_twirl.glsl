[[node]]
void node_twirl(float3 vector, float3 center, float amount, float3 &result)
{
  float uv_x = vector.x - center.x;
  float uv_y = vector.y - center.y;
  float radius = sqrt(uv_x * uv_x + uv_y * uv_y);
  float angle = atan(uv_y, uv_x);
  angle += radius * amount;

  result.x = cos(angle) * radius + center.x;
  result.y = sin(angle) * radius + center.y;
  result.z = vector.z;
}
