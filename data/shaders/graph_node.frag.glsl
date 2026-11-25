in vec2 v_local;
in vec3 v_color;
in float v_kind;
in float v_state;
in float v_alpha;
in float v_core_ratio;
in float v_px_radius;
in float v_depth_t;
out vec4 frag_color;

uniform vec3 u_glow_col;
uniform vec3 u_surface2_col;
uniform float u_hover_fade;
uniform int u_high_quality;
uniform int u_clean_style;
uniform vec3 u_bg_col;
uniform vec3 u_note_col;

void main()
{
  if (v_alpha < 0.01)
    discard;

  float aa = 1.25 / max(v_px_radius, 4.0);
  float d = length(v_local);
  if (d > 1.0)
    discard;

  bool is_foc = (v_state >= 1.5);
  bool is_nbr = (v_state >= 0.5 && v_state < 1.5);

  /* Clean (Obsidian-style) preset: flat antialiased dots + 1px background separation ring,
   * zero glow/halo so edges visually cut cleanly at node borders and dots never merge. */
  if (u_clean_style != 0) {
    float sep_px = 1.15 / max(v_px_radius, 4.0);
    float outer_r = v_core_ratio + sep_px;

    /* Session / Import: crisp small flat square with 1px bg separation outline */
    if (v_kind > 2.5 && v_kind < 4.5) {
      float hs = v_core_ratio * 0.92;
      vec2 q = abs(v_local) - vec2(hs);
      float sdf = max(q.x, q.y);
      float out_m = 1.0 - smoothstep(sep_px - aa, sep_px + aa, sdf);
      if (out_m < 0.01) discard;
      float in_m = 1.0 - smoothstep(-aa, aa, sdf);
      vec3 fill_c = is_foc ? u_glow_col : u_note_col;
      vec3 col = mix(u_bg_col, fill_c, in_m);
      frag_color = vec4(col, out_m * v_alpha);
      return;
    }

    /* Ghost: hollow circle with muted outline */
    if (v_kind >= 4.5 && v_kind < 5.5) {
      float ring = smoothstep(aa * 1.4, 0.0, abs(d - v_core_ratio));
      if (ring < 0.01) discard;
      vec3 col = is_foc ? u_glow_col : (u_note_col * 0.72);
      frag_color = vec4(col, ring * 0.85 * v_alpha);
      return;
    }

    float sep_m = 1.0 - smoothstep(outer_r - aa, outer_r + aa, d);
    if (sep_m < 0.01)
      discard;
    float fill_m = 1.0 - smoothstep(v_core_ratio - aa, v_core_ratio + aa, d);

    vec3 fill_c = u_note_col;
    if (is_foc) {
      fill_c = u_glow_col;
    } else if (v_kind >= 5.5) {
      /* Folder: crisp ring + small center in subtle neutral/folder tone */
      float inner_ring = smoothstep(aa * 1.35, 0.0, abs(d - v_core_ratio * 0.82));
      float center_dot = 1.0 - smoothstep(v_core_ratio * 0.42 - aa, v_core_ratio * 0.42 + aa, d);
      vec3 col = mix(u_bg_col, u_note_col, clamp(max(inner_ring, center_dot), 0.0, 1.0));
      frag_color = vec4(col, sep_m * v_alpha);
      return;
    } else if (v_kind >= 1.5 && v_kind < 2.5) {
      fill_c = mix(u_note_col, u_glow_col, 0.45);
    } else if (v_kind >= 0.5 && v_kind < 1.5) {
      /* AI note: note fill with accent outline */
      float ai_ring = smoothstep(aa * 1.3, 0.0, abs(d - (v_core_ratio - aa * 0.5)));
      fill_c = mix(u_note_col, u_glow_col, ai_ring);
    }

    vec3 col = mix(u_bg_col, fill_c, fill_m);
    frag_color = vec4(col, sep_m * v_alpha);
    return;
  }

  float ring_r = v_core_ratio + 3.0 * aa;

  /* Depth cue: farther nodes subtly desaturate and cool slightly */
  float luma = dot(v_color, vec3(0.299, 0.587, 0.114));
  vec3 base_col = mix(v_color, vec3(luma * 0.85, luma * 0.88, luma * 0.96), v_depth_t * 0.48);
  vec3 halo_tint = is_foc ? u_glow_col : base_col;

  /* Folder node (kind == 6): distinct luminous ring + soft glow + colored center */
  if (v_kind >= 5.5) {
    float outer_r = v_core_ratio * 0.95;
    float inner_r = v_core_ratio * 0.42;
    float ring = smoothstep(aa * 1.9, 0.0, abs(d - outer_r));
    float center = 1.0 - smoothstep(inner_r - aa, inner_r + aa, d);
    float halo = exp(-d * d * 5.2) * (is_foc ? 0.48 : (u_high_quality != 0 ? 0.24 : 0.10));
    vec3 col = base_col * (0.85 * ring + 0.95 * center) + halo_tint * halo;
    if (is_foc) {
      float hot = exp(-pow(d / max(inner_r, 0.01), 2.0)) * 0.65;
      col = mix(col, vec3(0.98, 0.98, 1.0), hot);
    }
    col = min(col, vec3(0.96));
    float a = clamp(max(max(ring * 0.95, center * 0.92), halo), 0.0, 1.0) * v_alpha;
    if (a < 0.005) discard;
    frag_color = vec4(col, a);
    return;
  }

  /* Session / Import root: rounded square badge with luminous border */
  if (v_kind > 2.5 && v_kind < 4.5) {
    float half_s = v_core_ratio * 1.05;
    vec2 q = abs(v_local) - vec2(half_s * 0.78);
    float sdf = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - half_s * 0.22;
    float fill_m = 1.0 - smoothstep(-aa, aa, sdf);
    float border_m = smoothstep(aa * 1.6, 0.0, abs(sdf));
    float dot_m = 1.0 - smoothstep(v_core_ratio * 0.25, v_core_ratio * 0.25 + aa, d);
    float halo = exp(-d * d * 5.5) * (is_foc ? 0.38 : (u_high_quality != 0 ? 0.14 : 0.0));
    vec3 col = mix(u_surface2_col, is_foc ? u_glow_col : base_col, max(border_m, dot_m));
    col = min(col + halo_tint * halo, vec3(0.95));
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
    float a = ring * 0.78 * v_alpha;
    if (a < 0.005) discard;
    frag_color = vec4(is_foc ? u_glow_col : base_col, a);
    return;
  }

  /* Soft round sprite with own color; white hot-center ONLY for hovered/selected */
  float halo_k = is_foc ? 4.8 : 7.2;
  float halo_str = (u_high_quality != 0)
      ? (is_foc ? (0.38 + 0.35 * u_hover_fade) : (is_nbr ? 0.26 : 0.16))
      : (is_foc ? (0.28 + 0.28 * u_hover_fade) : (is_nbr ? 0.16 : 0.06));
  float halo = exp(-d * d * halo_k) * halo_str;

  float core = 1.0 - smoothstep(v_core_ratio - aa, v_core_ratio + aa, d);

  float accent_ring = 0.0;
  if (is_foc) {
    float rw = aa * 1.5;
    accent_ring = smoothstep(rw, 0.0, abs(d - ring_r)) * (0.45 + 0.55 * u_hover_fade);
  }

  vec3 core_col = base_col;
  if (is_foc) {
    float hot = exp(-pow(d / max(v_core_ratio * 0.55, 0.01), 2.0)) * 0.72;
    core_col = mix(base_col, vec3(0.98, 0.99, 1.0), clamp(hot, 0.0, 0.72));
  }

  /* Normal alpha blend friendly color: never blow out to white on overlap */
  vec3 out_col = mix(halo_tint, core_col, core);
  out_col = mix(out_col, u_glow_col, clamp(accent_ring, 0.0, 1.0));
  out_col = min(out_col, is_foc ? vec3(1.0) : vec3(0.88));
  float out_a = clamp(max(core * 0.94, halo) + accent_ring * 0.9, 0.0, 1.0) * v_alpha;
  if (out_a < 0.004)
    discard;
  frag_color = vec4(out_col, out_a);
}
