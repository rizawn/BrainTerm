in vec2 v_local;
in vec3 v_color;
in float v_kind;
in float v_state;
in float v_alpha;
in float v_core_ratio;
in float v_px_radius;
out vec4 frag_color;

uniform vec3 u_glow_col;
uniform vec3 u_surface2_col;
uniform float u_hover_fade;
uniform int u_high_quality;

void main()
{
  if (v_alpha < 0.01)
    discard;

  float aa = 1.35 / max(v_px_radius, 4.0);
  float d = length(v_local);
  if (d > 1.0)
    discard;

  bool is_foc = (v_state >= 1.5);
  bool is_nbr = (v_state >= 0.5 && v_state < 1.5);
  float ring_r = v_core_ratio + 3.2 * aa;
  vec3 halo_tint = is_foc ? u_glow_col : v_color;

  /* Session / Import root: rounded square badge with luminous border */
  if (v_kind > 2.5 && v_kind < 4.5) {
    float half_s = v_core_ratio * 1.05;
    vec2 q = abs(v_local) - vec2(half_s * 0.78);
    float sdf = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - half_s * 0.22;
    float fill_m = 1.0 - smoothstep(-aa, aa, sdf);
    float border_m = smoothstep(aa * 1.6, 0.0, abs(sdf));
    float dot_m = 1.0 - smoothstep(v_core_ratio * 0.25, v_core_ratio * 0.25 + aa, d);
    float halo = exp(-d * d * 5.0) * (is_foc ? 0.45 : (u_high_quality != 0 ? 0.16 : 0.0));
    vec3 col = mix(u_surface2_col, is_foc ? u_glow_col : v_color, max(border_m, dot_m));
    col += halo_tint * halo;
    float a = max(max(fill_m, border_m), halo) * v_alpha;
    if (a < 0.005) discard;
    frag_color = vec4(col, a);
    return;
  }

  /* Ghost node: dashed circle */
  if (v_kind >= 4.5) {
    float ang = atan(v_local.y, v_local.x);
    float dash = step(0.0, sin(ang * 8.0));
    float ring = smoothstep(aa * 1.5, 0.0, abs(d - v_core_ratio)) * dash;
    float a = ring * 0.80 * v_alpha;
    if (a < 0.005) discard;
    frag_color = vec4(is_foc ? u_glow_col : v_color, a);
    return;
  }

  /* Luminous glowing dot: soft radial halo + crisp core + pinpoint center */
  float halo_k = is_foc ? 4.2 : 6.2;
  float halo_str = (u_high_quality != 0)
      ? (is_foc ? (0.45 + 0.50 * u_hover_fade) : (is_nbr ? 0.50 : 0.34))
      : (is_foc ? (0.35 + 0.40 * u_hover_fade) : (is_nbr ? 0.28 : 0.12));
  float halo = exp(-d * d * halo_k) * halo_str;

  float core = 1.0 - smoothstep(v_core_ratio - aa, v_core_ratio + aa, d);
  float pinpoint = exp(-pow(d / max(v_core_ratio * 0.55, 0.01), 2.0)) *
                   ((u_high_quality != 0) ? 0.55 : 0.25);

  float accent_ring = 0.0;
  if (is_foc) {
    float rw = aa * 1.5;
    accent_ring = smoothstep(rw, 0.0, abs(d - ring_r)) * (0.45 + 0.55 * u_hover_fade);
  }

  vec3 core_col = mix(v_color, vec3(0.96, 0.97, 1.0), clamp(pinpoint, 0.0, 0.75));
  vec3 out_col = halo_tint * halo + core_col * core + u_glow_col * accent_ring;
  float out_a = clamp(halo + core + accent_ring, 0.0, 1.0) * v_alpha;
  if (out_a < 0.004)
    discard;
  frag_color = vec4(out_col, out_a);
}
