layout(location = 0) in vec4 a_ab;   /* ax, ay, bx, by (world 2D) */
layout(location = 1) in vec4 a_meta; /* curve_sign, src_idx, dst_idx, kind */
layout(location = 2) in vec3 a_a3d;  /* 3D endpoint A */
layout(location = 3) in vec3 a_b3d;  /* 3D endpoint B */

uniform vec2 u_viewport;
uniform vec2 u_pan;
uniform float u_zoom;
uniform float u_scale;
uniform float u_birth_t;
uniform int u_focus_idx;
uniform int u_selected_idx;
uniform float u_hover_fade;
uniform int u_seg_count;
uniform int u_curved;
uniform float u_base_alpha;
uniform float u_half_width;
uniform vec3 u_dim_col;
uniform vec3 u_active_col;
uniform int u_mode_3d;
uniform mat4 u_mvp;
uniform int u_pulse_enabled;
uniform float u_pulse_phase;
uniform int u_show_links;
uniform float u_gal_time;

out float v_cross;
out float v_t;
out vec3 v_color;
out float v_alpha;
out float v_pulse;

void main()
{
  float bt = 1.0 - pow(1.0 - clamp(u_birth_t, 0.0, 1.0), 3.0);
  vec2 sa, sb;
  float depth_fade = 1.0;

  if (u_mode_3d != 0) {
    vec3 pa = a_a3d * bt;
    vec3 pb = a_b3d * bt;
    if (abs(u_gal_time) > 1e-5) {
      float ra = length(pa.xz);
      float aa = u_gal_time * (0.08 / (1.0 + ra / 0.3));
      float caa = cos(aa), saa = sin(aa);
      pa = vec3(pa.x * caa - pa.z * saa, pa.y, pa.x * saa + pa.z * caa);
      float rb = length(pb.xz);
      float ab = u_gal_time * (0.08 / (1.0 + rb / 0.3));
      float cab = cos(ab), sab = sin(ab);
      pb = vec3(pb.x * cab - pb.z * sab, pb.y, pb.x * sab + pb.z * cab);
    }
    vec4 ca = u_mvp * vec4(pa, 1.0);
    vec4 cb = u_mvp * vec4(pb, 1.0);
    if (ca.w <= 0.15 || cb.w <= 0.15) {
      gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
      v_cross = 0.0; v_t = 0.0; v_color = vec3(0.0); v_alpha = 0.0; v_pulse = 0.0;
      return;
    }
    vec2 na = ca.xy / ca.w;
    vec2 nb = cb.xy / cb.w;
    sa = vec2((na.x * 0.5 + 0.5) * u_viewport.x, (0.5 - na.y * 0.5) * u_viewport.y);
    sb = vec2((nb.x * 0.5 + 0.5) * u_viewport.x, (0.5 - nb.y * 0.5) * u_viewport.y);
    float avg_w = 0.5 * (ca.w + cb.w);
    depth_fade = clamp(1.42 - avg_w * 0.24, 0.22, 1.0);
  } else {
    vec2 wa = a_ab.xy * bt;
    vec2 wb = a_ab.zw * bt;
    sa = u_viewport * 0.5 + u_pan + wa * u_zoom;
    sb = u_viewport * 0.5 + u_pan + wb * u_zoom;
  }

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
  bool is_src_foc = has_focus && (abs(a_meta.y - float(u_focus_idx)) < 0.5);
  bool is_dst_foc = has_focus && (abs(a_meta.z - float(u_focus_idx)) < 0.5);
  bool is_active = is_src_foc || is_dst_foc;
  bool is_tether = (a_meta.w >= 3.5); /* BT_EDGE_CONTAINS */

  if (!is_tether && !is_active && u_show_links == 0) {
    gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
    v_cross = 0.0; v_t = 0.0; v_color = vec3(0.0); v_alpha = 0.0; v_pulse = 0.0;
    return;
  }

  bool has_sel = (u_selected_idx >= 0);
  bool is_src_sel = has_sel && (abs(a_meta.y - float(u_selected_idx)) < 0.5);
  bool is_dst_sel = has_sel && (abs(a_meta.z - float(u_selected_idx)) < 0.5);

  float pulse_w = 0.0;
  if (u_pulse_enabled != 0 && !is_tether && (is_src_sel || is_dst_sel)) {
    float t_out = is_src_sel ? t : (1.0 - t);
    float diff = t_out - u_pulse_phase;
    pulse_w = exp(-diff * diff * 72.0);
  }

  float dim_factor = has_focus ? (1.0 - 0.78 * u_hover_fade) : 1.0;
  float hw = u_half_width * u_scale *
             (is_active ? (1.35 + 0.45 * u_hover_fade) : (is_tether ? 0.65 : (1.0 + 0.75 * pulse_w)));
  hw = max(hw, 0.70 * u_scale);

  vec2 px = pos + norm * (side * hw);
  vec2 ndc = (px / u_viewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);

  v_cross = side;
  v_t = t;
  v_color = (is_active || pulse_w > 0.12) ? u_active_col : u_dim_col;
  float base_a = is_tether ? (u_base_alpha * 0.42 * dim_factor) : (u_base_alpha * dim_factor);
  v_alpha = (is_active ? (0.32 + 0.62 * u_hover_fade) : base_a) * depth_fade;
  v_pulse = pulse_w * depth_fade;
  if (len < 1.0)
    v_alpha = 0.0;
}
