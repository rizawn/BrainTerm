in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_tex;
uniform vec2 u_dir; /* (1/w, 0) for horizontal pass, (0, 1/h) for vertical pass */

void main()
{
  /* 9-tap separable Gaussian via 5 bilinear samples */
  vec3 sum = texture(u_tex, v_uv).rgb * 0.227027;
  sum += texture(u_tex, v_uv + u_dir * 1.384615).rgb * 0.316216;
  sum += texture(u_tex, v_uv - u_dir * 1.384615).rgb * 0.316216;
  sum += texture(u_tex, v_uv + u_dir * 3.230769).rgb * 0.070270;
  sum += texture(u_tex, v_uv - u_dir * 3.230769).rgb * 0.070270;
  frag_color = vec4(sum, 1.0);
}
