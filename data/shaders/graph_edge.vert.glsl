layout(location = 0) in vec4 a_ab;   /* ax, ay, bx, by (world) */
layout(location = 1) in vec4 a_meta; /* curve_sign, src_idx, dst_idx, kind */

uniform vec2 u_viewport;
uniform vec2 u_pan;
uniform float u_zoom;
uniform float u_scale;
uniform float u_birth_t;
uniform int u_focus_idx;
uniform float u_hover_fade;
uniform int u_seg_count;
uniform int u_curved;
uniform float u_base_alpha;
uniform float u_half_width;
uniform vec3 u_dim_col;
uniform vec3 u_active_col;

out float v_cross;
out float v_t;
out vec3 v_color;
out float v_alpha;

void main()
{
  float bt = 1.0 - pow(1.0 - clamp(u_birth_t, 0.0, 1.0), 3.0);
  vec2 wa = a_ab.xy * bt;
  vec2 wb = a_ab.zw * bt;

  vec2 sa = u_viewport * 0.5 + u_pan + wa * u_zoom;
  vec2 sb = u_viewport * 0.5 + u_pan + wb * u_zoom;

  int seg = gl_VertexID / 2;
  float side = ((gl_VertexID & 1) != 0) ? 1.0 : -1.0;
  float t = float(seg) / max(float(u_seg_count), 1.0);

  vec2 d = sb - sa;
  float len = max(length(d), 0.001);
  vec2 mid = (sa + sb) * 0.5;
  vec2 ctrl = mid + vec2(-d.y, d.x) * (a_meta.x * 0.11 * float(u_curved));

  float omt = 1.0 - t;
  vec2 pos = omt * omt * sa + 2.0 * omt * t * ctrl + t * t * sb;
  vec2 tang = 2.0 * omt * (ctrl - sa) + 2.0 * t * (sb - ctrl);
  float tlen = max(length(tang), 0.001);
  vec2 norm = vec2(-tang.y, tang.x) / tlen;

  bool has_focus = (u_focus_idx >= 0);
  bool is_active = has_focus &&
      (abs(a_meta.y - float(u_focus_idx)) < 0.5 || abs(a_meta.z - float(u_focus_idx)) < 0.5);

  float dim_factor = has_focus ? (1.0 - 0.75 * u_hover_fade) : 1.0;
  float hw = u_half_width * u_scale * (is_active ? (1.35 + 0.45 * u_hover_fade) : 1.0);
  /* Ensure crisp sub-pixel minimum width without vanishing */
  hw = max(hw, 0.85 * u_scale);

  vec2 px = pos + norm * (side * hw);
  vec2 ndc = (px / u_viewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);

  v_cross = side;
  v_t = t;
  v_color = is_active ? u_active_col : u_dim_col;
  v_alpha = is_active ? (0.30 + 0.65 * u_hover_fade) : (u_base_alpha * dim_factor);
  if (len < 1.0)
    v_alpha = 0.0;
}
