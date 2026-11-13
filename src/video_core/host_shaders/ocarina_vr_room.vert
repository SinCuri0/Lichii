#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 out_uv;

layout(push_constant) uniform Eye {
    mat4 view_projection;
} eye;

void main() {
    out_uv = uv;
    gl_Position = eye.view_projection * vec4(position, 1);
}
