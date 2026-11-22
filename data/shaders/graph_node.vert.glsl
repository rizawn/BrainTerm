layout(location = 0) in vec4 a_geom;  /* x, y, radius, kind */
layout(location = 1) in vec4 a_color; /* r, g, b, visible */
layout(location = 2) in float a_state;/* 0=none, 1=nbr, 2=focus, 3=sel, 4=kbd */
layout(location = 3) in vec3 a_pos3d; /* normalized 3D cortex coords */

uniform vec2 u_viewport;
uniform vec2 u_pan;
uniform float u_zoom;
uniform float u_scale;
uniform float u_birth_t;
uniform float u_has_focus;
uniform float u_hover_fade;
uniform int u_high_quality;
uniform int u_mode_3d;
uniform mat4 u_mvp;

out vec2 v_local;
out vec3 v_color;
out float v_kind;
out float v_state;
out float v_alpha;
out float v_core_ratio;
out float v_px_radius;

void main()
{
  if (a_color.w < 0.5) {
    gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
    v_local = vec2(0.0); v_color = vec3(0.0); v_kind = 0.0;
    v_state = 0.0; v_alpha = 0.0; v_core_ratio = 0.3; v_px_radius = 1.0;
    return;
  }

  vec2 corner = vec2((gl_VertexID & 1) != 0 ? 1.0 : -1.0,
                     (gl_VertexID & 2) != 0 ? 1.0 : -1.0);

  float bt = 1.0 - pow(1.0 - clamp(u_birth_t, 0.0, 1.0), 3.0);
  vec2 center_px;
  float core_px;
  float depth_fade = 1.0;

  if (u_mode_3d != 0) {
    vec4 clip = u_mvp * vec4(a_pos3d * bt, 1.0);
    if (clip.w <= 0.15) {
      gl_Position = vec4(-2.0, -2.0, 0.0, 1.0);
      v_local = vec2(0.0); v_color = vec3(0.0); v_kind = 0.0;
      v_state = 0.0; v_alpha = 0.0; v_core_ratio = 0.3; v_px_radius = 1.0;
      return;
    }
    vec2 ndc2 = clip.xy / clip.w;
    center_px = vec2((ndc2.x * 0.5 + 0.5) * u_viewport.x,
                     (0.5 - ndc2.y * 0.5) * u_viewport.y);
    float persp = clamp(2.4 / max(clip.w, 0.35), 0.35, 2.4);
    core_px = clamp(a_geom.z * persp * 0.85, 2.0, 14.0) * u_scale;
    depth_fade = clamp(1.42 - clip.w * 0.22, 0.28, 1.0);
  } else {
    vec2 wpos = a_geom.xy * bt;
    center_px = u_viewport * 0.5 + u_pan + wpos * u_zoom;
    core_px = clamp(a_geom.z * pow(max(u_zoom, 0.05), 0.35), 2.2, 14.0) * u_scale;
  }

  float halo_mult = (u_high_quality != 0) ? 2.4 : 1.8;
  if (a_state >= 1.5)
    halo_mult = 2.8;
  float quad_px = min(core_px * halo_mult + 2.0 * u_scale, 30.0 * u_scale);

  vec2 px = center_px + corner * quad_px;
  vec2 ndc = (px / u_viewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);

  float dim_factor = (u_has_focus > 0.5 && a_state < 0.5) ? (1.0 - 0.75 * u_hover_fade) : 1.0;

  v_local = corner;
  v_color = a_color.rgb;
  v_kind = a_geom.w;
  v_state = a_state;
  v_alpha = dim_factor * ((a_state >= 1.5) ? 1.0 : depth_fade);
  v_core_ratio = core_px / max(quad_px, 1.0);
  v_px_radius = quad_px;
}
