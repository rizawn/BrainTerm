in vec2 v_uv;
in vec2 v_local;
in float v_alpha;
in float v_pill;
out vec4 frag_color;

uniform sampler2D u_atlas;
uniform vec3 u_text_primary;
uniform vec3 u_text_secondary;
uniform vec3 u_pill_bg;
uniform vec3 u_bg_col;

void main()
{
  vec4 tex = texture(u_atlas, v_uv);
  float glyph_a = tex.a;
  vec3 fg = mix(u_text_secondary, u_text_primary, clamp(v_pill, 0.0, 1.0));

  /* Soft corner mask for clean pill backdrop */
  vec2 edge = min(v_local, vec2(1.0) - v_local);
  float corner = smoothstep(0.0, 0.06, min(edge.x * 2.5, edge.y));

  if (v_pill > 0.5) {
    /* Primary Dusk pill backdrop behind focused/selected/hovered label */
    vec3 col = mix(u_pill_bg, fg, glyph_a);
    float a = max(0.90 * corner, glyph_a) * v_alpha;
    if (a < 0.01)
      discard;
    frag_color = vec4(col, a);
  } else {
    /* Subtle @bt_graph_bg ~70% translucent backdrop behind all drawn labels */
    vec3 col = mix(u_bg_col, fg, glyph_a);
    float a = max(0.70 * corner, glyph_a) * v_alpha;
    if (a < 0.01)
      discard;
    frag_color = vec4(col, a);
  }
}
