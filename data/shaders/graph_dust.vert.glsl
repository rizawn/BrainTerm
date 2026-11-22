layout(location = 0) in vec4 a_pos_vel;   /* x0, y0, vx, vy in [0,1] */
layout(location = 1) in vec2 a_size_alpha;/* radius_px, alpha */

uniform vec2 u_viewport;
uniform float u_time;
uniform float u_scale;

out vec2 v_local;
out float v_alpha;

void main()
{
  vec2 corner = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0,
                     (gl_VertexID & 2) != 0 ? 1.0 : -1.0);
  vec2 uv = fract(a_pos_vel.xy + a_pos_vel.zw * u_time);
  vec2 center_px = uv * u_viewport;
  float r_px = a_size_alpha.x * u_scale * 2.2;
  vec2 px = center_px + corner * r_px;

  vec2 ndc = (px / u_viewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);
  v_local = corner;
  v_alpha = a_size_alpha.y;
}
