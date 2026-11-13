#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(binding = 0) uniform sampler2D room;

void main() {
    color = vec4(texture(room, uv).rgb, 1);
}
