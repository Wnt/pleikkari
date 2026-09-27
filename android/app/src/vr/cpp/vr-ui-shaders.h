// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// PLE-722: the VR UI's footprint, laser and reticle shaders (vr-ui-layers.cpp). Plain GLSL ES 3.00
// with no VrApi, so a host check can compile and link them through Mesa.
#ifndef PLEIKKARI_VR_UI_SHADERS_H
#define PLEIKKARI_VR_UI_SHADERS_H

namespace pleikkari {

inline const char *const VrUiVertexShader = R"(#version 300 es
layout(location=0) in vec2 corner;
uniform mat4 mvp;
uniform mediump int mode; // one precision in both stages, or the program does not link
uniform vec3 a;
uniform vec3 b;
uniform vec3 c;
out vec2 uv;
void main() {
    vec3 p;
    if(mode == 0) {
        // Footprint: a = (arc, height over radius, radius), in the panel's own frame.
        float theta = (corner.x - 0.5) * a.x;
        p = vec3(a.z * sin(theta), (0.5 - corner.y) * a.y * a.z, -a.z * cos(theta));
    } else if(mode == 1) {
        p = mix(a, b, corner.x) + c * (corner.y - 0.5); // laser: start, end, width across
    } else {
        p = a + b * (corner.x * 2.0 - 1.0) + c * (corner.y * 2.0 - 1.0); // reticle: centre, right, up
    }
    uv = corner;
    gl_Position = mvp * vec4(p, 1.0);
})";

inline const char *const VrUiFragmentShader = R"(#version 300 es
precision mediump float;
uniform mediump int mode; // one precision in both stages, or the program does not link
uniform vec4 color;
uniform vec4 rect;   // footprint: texture size in texels, border inset, corner radius
uniform float opacity;
in vec2 uv;
out vec4 result;
void main() {
    if(mode == 0) {
        // The same rounded rectangle the toolkit fills, as a signed distance in texels.
        vec2 p = uv * rect.xy - rect.xy * 0.5;
        vec2 q = abs(p) - (rect.xy * 0.5 - vec2(rect.z + rect.w));
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - rect.w;
        result = vec4(0.0, 0.0, 0.0, clamp(0.5 - d, 0.0, 1.0) * opacity);
    } else if(mode == 1) {
        float along = 1.0 - smoothstep(0.1, 0.9, uv.x);
        float across = 1.0 - abs(uv.y * 2.0 - 1.0);
        result = vec4(color.rgb, color.a * along * smoothstep(0.0, 0.5, across));
    } else {
        float r = length(uv * 2.0 - 1.0);
        float disc = 1.0 - smoothstep(0.58, 0.70, r);
        float outline = 1.0 - smoothstep(0.86, 1.0, r);
        result = vec4(mix(vec3(0.07, 0.07, 0.09), color.rgb, disc), color.a * outline);
    }
})";

} // namespace pleikkari

#endif
