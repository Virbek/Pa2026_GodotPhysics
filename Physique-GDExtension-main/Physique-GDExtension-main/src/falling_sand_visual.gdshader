shader_type canvas_item;
render_mode unshaded;

float marker_id(vec2 uv) {
    vec3 marker = texture(TEXTURE, clamp(uv, vec2(0.0), vec2(1.0))).rgb;

    if (marker.r > 0.5) {
        return 1.0;
    }
    if (marker.g > 0.5) {
        return 2.0;
    }
    if (marker.b > 0.5) {
        return 3.0;
    }
    return 0.0;
}

float hash21(vec2 point) {
    point = fract(point * vec2(123.34, 456.21));
    point += dot(point, point + 45.32);
    return fract(point.x * point.y);
}

void fragment() {
    vec2 texel = TEXTURE_PIXEL_SIZE;
    vec2 cell = floor(UV / texel);
    float id = marker_id(UV);

    float background_noise = hash21(floor(cell * 0.25)) * 0.012;
    vec3 final_color = vec3(0.008, 0.012, 0.025) + background_noise;

    if (id == 1.0) {
        float grain = hash21(cell);
        float bands = sin(cell.y * 0.17 + grain * 4.0) * 0.035;
        final_color = vec3(0.93, 0.48, 0.16) * mix(0.78, 1.13, grain) + bands;
    } else if (id == 2.0) {
        float wave = sin(cell.x * 0.32 + TIME * 3.2 + sin(cell.y * 0.11)) * 0.5 + 0.5;
        float sparkle = pow(hash21(cell + floor(TIME * 3.0)), 18.0) * 0.18;
        final_color = mix(
            vec3(0.015, 0.20, 0.55),
            vec3(0.03, 0.64, 0.96),
            0.28 + wave * 0.28
        );
        final_color += sparkle;
    } else if (id == 3.0) {
        float stone_noise = hash21(cell * 0.73);
        float crack = step(0.94, hash21(cell * 1.91 + vec2(7.0, 3.0)));
        final_color = mix(
            vec3(0.18, 0.19, 0.23),
            vec3(0.42, 0.44, 0.50),
            stone_noise
        );
        final_color *= 1.0 - crack * 0.42;
    }

    if (id > 0.5) {
        float edge = 0.0;
        edge = max(edge, step(0.5, abs(id - marker_id(UV + vec2(texel.x, 0.0)))));
        edge = max(edge, step(0.5, abs(id - marker_id(UV - vec2(texel.x, 0.0)))));
        edge = max(edge, step(0.5, abs(id - marker_id(UV + vec2(0.0, texel.y)))));
        edge = max(edge, step(0.5, abs(id - marker_id(UV - vec2(0.0, texel.y)))));
        final_color *= mix(1.0, 0.72, edge);
    }

    // Bright line on the top surface of water.
    if (id == 2.0) {
        float above_id = marker_id(UV - vec2(0.0, texel.y));
        float water_surface = step(0.5, abs(above_id - 2.0));
        final_color += vec3(0.22, 0.52, 0.72) * water_surface;
    }

    float distance_from_center = length(UV - vec2(0.5));
    float vignette = 1.0 - smoothstep(0.32, 0.78, distance_from_center);
    final_color *= mix(0.76, 1.0, vignette);

    COLOR = vec4(final_color, 1.0);
}
