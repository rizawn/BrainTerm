in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform sampler2D u_bloom;
uniform float u_strength;

void main()
{
  vec3 base = texture(u_scene, v_uv).rgb;
  vec3 glow = texture(u_bloom, v_uv).rgb * u_strength;
  vec3 col = base + glow;
  /* Gentle shoulder roll-off keeps bright clusters luminous without harsh clipping */
  col = col / (vec3(1.0) + max(col - vec3(0.88), vec3(0.0)) * 0.55);
  frag_color = vec4(clamp(col, 0.0, 1.0), 1.0);
}
