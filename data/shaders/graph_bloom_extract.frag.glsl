in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_scene;
uniform float u_threshold;

void main()
{
  vec3 c = texture(u_scene, v_uv).rgb;
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float knee = 0.12;
  float soft = clamp((luma - u_threshold + knee) / (2.0 * knee), 0.0, 1.0);
  soft = soft * soft * (3.0 - 2.0 * soft);
  float contrib = max(luma - u_threshold, 0.0) + soft * knee * 0.5;
  float scale = contrib / max(luma, 0.0001);
  frag_color = vec4(c * scale, 1.0);
}
