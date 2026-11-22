layout(location = 0) in vec4 a_rect;  /* x, y, w, h in screen px */
layout(location = 1) in vec4 a_uv;    /* u0, v0, u1, v1 */
layout(location = 2) in vec2 a_style; /* alpha, pill_flag */

uniform vec2 u_viewport;

out vec2 v_uv;
out vec2 v_local;
out float v_alpha;
out float v_pill;

void main()
{
  vec2 c01 = vec2((gl_VertexID & 1) != 0 ? 1.0 : 0.0,
                  (gl_VertexID & 2) != 0 ? 1.0 : 0.0);
  vec2 px = a_rect.xy + c01 * a_rect.zw;
  vec2 ndc = (px / u_viewport) * 2.0 - 1.0;
  ndc.y = -ndc.y;
  gl_Position = vec4(ndc, 0.0, 1.0);

  v_uv = mix(a_uv.xy, a_uv.zw, c01);
  v_local = c01;
  v_alpha = a_style.x;
  v_pill = a_style.y;
}
