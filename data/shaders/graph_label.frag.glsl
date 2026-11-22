in vec2 v_uv;
in vec2 v_local;
in float v_alpha;
in float v_pill;
out vec4 frag_color;

uniform sampler2D u_atlas;
uniform vec3 u_text_primary;
uniform vec3 u_text_secondary;
uniform vec3 u_pill_bg;

void main()
{
  vec4 tex = texture(u_atlas, v_uv);
  float glyph_a = tex.a;
  vec3 fg = mix(u_text_secondary, u_text_primary, clamp(v_pill, 0.0, 1.0));

  if (v_pill > 0.5) {
    /* Subtle Dusk pill backdrop behind hovered/selected label */
    vec3 col = mix(u_pill_bg, fg, glyph_a);
    float a = max(0.86, glyph_a) * v_alpha;
    frag_color = vec4(col, a);
  } else {
    float a = glyph_a * v_alpha;
    if (a < 0.01)
      discard;
    frag_color = vec4(fg, a);
  }
}
