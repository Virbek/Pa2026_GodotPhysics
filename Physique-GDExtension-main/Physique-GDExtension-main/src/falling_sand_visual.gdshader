shader_type canvas_item;
render_mode unshaded;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

int material_id(vec2 uv) {
    float encoded = texture(TEXTURE, clamp(uv, vec2(0.0), vec2(1.0))).r;
    return int(encoded * 255.0 + 0.5) % 128;
}

void fragment() {
    ivec2 texture_size = textureSize(TEXTURE, 0);
    vec2 texel = 1.0 / vec2(texture_size);
    vec2 cell = floor(UV * vec2(texture_size));
    int id = material_id(UV);

    float noise = hash21(cell);
    vec3 color = vec3(0.012, 0.015, 0.026) + noise * 0.010;

    if (id == 1) {
        float bands = sin(cell.y * 0.16 + noise * 5.0) * 0.035;
        color = vec3(0.94, 0.53, 0.20) * mix(0.78, 1.12, noise) + bands;
    } else if (id == 2) {
        float wave = sin(cell.x * 0.28 + TIME * 3.4 + sin(cell.y * 0.10)) * 0.5 + 0.5;
        color = mix(vec3(0.015, 0.19, 0.54), vec3(0.04, 0.66, 0.98), 0.25 + wave * 0.34);
    } else if (id == 3) {
        float crack = step(0.95, hash21(cell * 1.91 + vec2(7.0, 3.0)));
        color = mix(vec3(0.17, 0.18, 0.22), vec3(0.46, 0.47, 0.52), noise);
        color *= 1.0 - crack * 0.45;
    } else if (id == 4) {
        float swirl = sin(TIME * 1.8 + cell.y * 0.17 + noise * 6.2831) * 0.08;
        color = vec3(0.25, 0.27, 0.31) * (0.72 + noise * 0.34 + swirl);
    } else if (id == 5) {
        float flicker = 0.72 + 0.28 * hash21(cell + floor(TIME * 18.0));
        float flame = sin(cell.x * 0.8 + TIME * 8.0 + noise * 5.0) * 0.5 + 0.5;
        color = mix(vec3(1.0, 0.16, 0.015), vec3(1.0, 0.93, 0.18), flame) * flicker;
    } else if (id == 6) {
        float grain = sin(cell.x * 0.34 + noise * 3.0) * 0.5 + 0.5;
        color = mix(vec3(0.20, 0.075, 0.025), vec3(0.52, 0.25, 0.07), grain);
    } else if (id == 7) {
        float pulse = sin(TIME * 3.0 + cell.x * 0.16 + cell.y * 0.09) * 0.5 + 0.5;
        color = mix(vec3(0.70, 0.025, 0.005), vec3(1.0, 0.55, 0.03), 0.25 + pulse * 0.60);
        color += vec3(0.18, 0.035, 0.0);
    } else if (id == 8) {
        float acid_wave = sin(TIME * 4.0 + cell.x * 0.24 + noise * 4.0) * 0.5 + 0.5;
        color = mix(vec3(0.12, 0.52, 0.015), vec3(0.62, 1.0, 0.04), acid_wave);
    } else if (id == 9) {
    float sheen = sin(cell.x * 0.22 + TIME * 1.7 + noise * 6.0) * 0.5 + 0.5;
    color = mix(
        vec3(0.12, 0.085, 0.025),
        vec3(0.55, 0.38, 0.09),
        0.20 + sheen * 0.65
    );
    } else if (id == 10) {
        float vapor = sin(TIME * 2.2 + cell.x * 0.12 + cell.y * 0.19 + noise * 6.0) * 0.08;
        color = vec3(0.70, 0.82, 0.90) * (0.80 + noise * 0.20 + vapor);
    }

    if (id > 0) {
        float edge = 0.0;
        edge = max(edge, material_id(UV + vec2(texel.x, 0.0)) != id ? 1.0 : 0.0);
        edge = max(edge, material_id(UV - vec2(texel.x, 0.0)) != id ? 1.0 : 0.0);
        edge = max(edge, material_id(UV + vec2(0.0, texel.y)) != id ? 1.0 : 0.0);
        edge = max(edge, material_id(UV - vec2(0.0, texel.y)) != id ? 1.0 : 0.0);
        color *= mix(1.0, 0.76, edge);
    }

    if (id == 2 && material_id(UV - vec2(0.0, texel.y)) != 2) {
        color += vec3(0.20, 0.46, 0.67);
    }

    float center_distance = length(UV - vec2(0.5));
    float vignette = 1.0 - smoothstep(0.34, 0.78, center_distance);
    color *= mix(0.78, 1.0, vignette);

    COLOR = vec4(color, 1.0);
}
