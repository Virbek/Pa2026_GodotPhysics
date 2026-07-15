#include "falling_sand_grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>

using namespace godot;

static_assert(FallingSandGrid::WORLD_WIDTH % FallingSandGrid::CHUNK_SIZE == 0,
              "WORLD_WIDTH doit etre un multiple de CHUNK_SIZE");
static_assert(FallingSandGrid::WORLD_HEIGHT % FallingSandGrid::CHUNK_SIZE == 0,
              "WORLD_HEIGHT doit etre un multiple de CHUNK_SIZE");
static_assert(FallingSandGrid::MAX_MOVE_DISTANCE * 2 + 2 < FallingSandGrid::CHUNK_SIZE,
              "MAX_MOVE_DISTANCE trop grand pour la securite du damier multithread");

namespace {

constexpr uint8_t LIQUID_SETTLE_FRAMES = 24;

inline uint32_t rng_next() {
    thread_local uint32_t state = 0;
    if (state == 0) {
        const uint64_t pointer_value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&state));
        state = static_cast<uint32_t>(pointer_value ^ (pointer_value >> 32)) * 2654435761u;
        state ^= 0x9E3779B9u;
        if (state == 0) {
            state = 1;
        }
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

inline bool rng_bool() {
    return (rng_next() & 1u) != 0u;
}

inline bool rng_chance(uint32_t numerator, uint32_t denominator) {
    return denominator != 0u && (rng_next() % denominator) < numerator;
}

inline uint8_t rng_u8(uint8_t minimum, uint8_t maximum) {
    const uint32_t range = static_cast<uint32_t>(maximum - minimum) + 1u;
    return static_cast<uint8_t>(minimum + (rng_next() % range));
}

inline void atomic_fetch_min(std::atomic<int> &value, int candidate) {
    int current = value.load(std::memory_order_relaxed);
    while (candidate < current &&
           !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

inline void atomic_fetch_max(std::atomic<int> &value, int candidate) {
    int current = value.load(std::memory_order_relaxed);
    while (candidate > current &&
           !value.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
    }
}

const char *FALLING_SAND_SHADER = R"SHADER(
shader_type canvas_item;
render_mode unshaded;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

int decode_material(float encoded) {
    return int(encoded * 255.0 + 0.5) % 128;
}

void fragment() {
    ivec2 texture_size = textureSize(TEXTURE, 0);
    vec2 texel = 1.0 / vec2(texture_size);
    vec2 cell = floor(UV * vec2(texture_size));
    int id = decode_material(texture(TEXTURE, clamp(UV, vec2(0.0), vec2(1.0))).r);

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
        color = mix(vec3(0.045, 0.035, 0.025), vec3(0.30, 0.20, 0.045), sheen * 0.55);
    } else if (id == 10) {
        float vapor = sin(TIME * 2.2 + cell.x * 0.12 + cell.y * 0.19 + noise * 6.0) * 0.08;
        color = vec3(0.70, 0.82, 0.90) * (0.80 + noise * 0.20 + vapor);
    }

    if (id > 0) {
        float edge = 0.0;
        edge = max(edge, decode_material(texture(TEXTURE, clamp(UV + vec2(texel.x, 0.0), vec2(0.0), vec2(1.0))).r) != id ? 1.0 : 0.0);
        edge = max(edge, decode_material(texture(TEXTURE, clamp(UV - vec2(texel.x, 0.0), vec2(0.0), vec2(1.0))).r) != id ? 1.0 : 0.0);
        edge = max(edge, decode_material(texture(TEXTURE, clamp(UV + vec2(0.0, texel.y), vec2(0.0), vec2(1.0))).r) != id ? 1.0 : 0.0);
        edge = max(edge, decode_material(texture(TEXTURE, clamp(UV - vec2(0.0, texel.y), vec2(0.0), vec2(1.0))).r) != id ? 1.0 : 0.0);
        color *= mix(1.0, 0.76, edge);
    }

    if (id == 2 && decode_material(texture(TEXTURE, clamp(UV - vec2(0.0, texel.y), vec2(0.0), vec2(1.0))).r) != 2) {
        color += vec3(0.20, 0.46, 0.67);
    }

    float center_distance = length(UV - vec2(0.5));
    float vignette = 1.0 - smoothstep(0.34, 0.78, center_distance);
    color *= mix(0.78, 1.0, vignette);

    COLOR = vec4(color, 1.0);
}
)SHADER";

Ref<StyleBoxFlat> make_button_style(const Color &base, bool selected, float lighten) {
    Ref<StyleBoxFlat> style;
    style.instantiate();
    style->set_bg_color(selected ? base.lightened(0.28f + lighten) : base.lightened(lighten));
    if (selected) {
        style->set_border_width_all(3);
        style->set_border_color(Color(1.0f, 1.0f, 1.0f));
    }
    return style;
}

struct ParticleInfo {
    Particle type;
    const char *name;
    Color color;
};

const ParticleInfo PARTICLE_INFOS[] = {
    {SAND, "Sable", Color(0.96f, 0.64f, 0.38f)},
    {WATER, "Eau", Color(0.0f, 0.62f, 1.0f)},
    {ROCK, "Roche", Color(0.41f, 0.41f, 0.44f)},
    {SMOKE, "Fumee", Color(0.30f, 0.31f, 0.34f)},
    {FIRE, "Feu", Color(1.0f, 0.32f, 0.03f)},
    {WOOD, "Bois", Color(0.46f, 0.22f, 0.06f)},
    {LAVA, "Lave", Color(1.0f, 0.16f, 0.01f)},
    {ACID, "Acide", Color(0.42f, 0.95f, 0.03f)},
    {OIL, "Huile", Color(0.24f, 0.17f, 0.04f)},
    {STEAM, "Vapeur", Color(0.72f, 0.84f, 0.92f)},
    {EMPTY, "Effacer", Color(0.12f, 0.12f, 0.14f)},
};

constexpr int PARTICLE_INFO_COUNT = static_cast<int>(sizeof(PARTICLE_INFOS) / sizeof(PARTICLE_INFOS[0]));

const char *particle_name(Particle type) {
    for (const ParticleInfo &info : PARTICLE_INFOS) {
        if (info.type == type) {
            return info.name;
        }
    }
    return "Inconnu";
}

} // namespace

// ===========================================================================
// Cycle de vie
// ===========================================================================

FallingSandGrid::FallingSandGrid() {
    const size_t cell_count = static_cast<size_t>(WORLD_WIDTH) * WORLD_HEIGHT;
    grid.assign(cell_count, static_cast<uint8_t>(EMPTY));
    cell_data.assign(cell_count, 0);
    chunks = std::make_unique<Chunk[]>(NUM_CHUNKS);
}

FallingSandGrid::~FallingSandGrid() = default;

void FallingSandGrid::_bind_methods() {
    ClassDB::bind_method(D_METHOD("on_particle_button_pressed", "particle_type"),
                         &FallingSandGrid::on_particle_button_pressed);
    ClassDB::bind_method(D_METHOD("_simulate_chunk", "list_index"),
                         &FallingSandGrid::_simulate_chunk);
}

void FallingSandGrid::_ready() {
    DisplayServer::get_singleton()->window_set_size(
            Vector2i(WORLD_WIDTH * CELL_SIZE, WORLD_HEIGHT * CELL_SIZE + UI_HEIGHT));

    pixel_bytes.resize(static_cast<int64_t>(WORLD_WIDTH) * WORLD_HEIGHT);
    image = Image::create_from_data(WORLD_WIDTH, WORLD_HEIGHT, false, Image::FORMAT_R8, pixel_bytes);
    texture = ImageTexture::create_from_image(image);

    sprite = memnew(Sprite2D);
    sprite->set_texture(texture);
    sprite->set_centered(false);
    sprite->set_scale(Vector2(static_cast<real_t>(CELL_SIZE), static_cast<real_t>(CELL_SIZE)));
    sprite->set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
    sprite->set_draw_behind_parent(true);
    add_child(sprite);

    setup_material();
    setup_ui();
    render_grid();
}

void FallingSandGrid::setup_material() {
    Ref<Shader> shader;
    shader.instantiate();
    shader->set_code(FALLING_SAND_SHADER);

    Ref<ShaderMaterial> material;
    material.instantiate();
    material->set_shader(shader);
    sprite->set_material(material);
}

// ===========================================================================
// Proprietes des materiaux
// ===========================================================================

bool FallingSandGrid::is_solid(Particle type) {
    return type == ROCK || type == WOOD;
}

bool FallingSandGrid::is_liquid(Particle type) {
    return type == WATER || type == LAVA || type == ACID || type == OIL;
}

bool FallingSandGrid::is_gas(Particle type) {
    return type == SMOKE || type == FIRE || type == STEAM;
}

bool FallingSandGrid::is_flammable(Particle type) {
    return type == WOOD || type == OIL;
}

int FallingSandGrid::density(Particle type) {
    switch (type) {
        case EMPTY: return -1000;
        case FIRE: return -40;
        case SMOKE: return -30;
        case STEAM: return -20;
        case OIL: return 10;
        case WATER: return 20;
        case ACID: return 26;
        case LAVA: return 32;
        case SAND: return 50;
        case ROCK:
        case WOOD: return 10000;
        default: return 0;
    }
}

int FallingSandGrid::liquid_dispersion(Particle type) {
    switch (type) {
        case WATER: return WATER_DISPERSION;
        case OIL: return OIL_DISPERSION;
        case ACID: return ACID_DISPERSION;
        case LAVA: return LAVA_DISPERSION;
        default: return 1;
    }
}

bool FallingSandGrid::can_sink_into(Particle moving, Particle target) const {
    if (target == EMPTY) {
        return true;
    }
    if (moving == target || is_solid(target) || target == SAND) {
        return false;
    }
    return density(target) < density(moving);
}

bool FallingSandGrid::can_gas_rise_into(Particle moving, Particle target) const {
    if (target == EMPTY) {
        return true;
    }
    return is_gas(target) && target != moving && density(target) > density(moving);
}

uint8_t FallingSandGrid::initial_cell_data(Particle type) const {
    switch (type) {
        case FIRE: return rng_u8(48, 92);
        case SMOKE: return rng_u8(100, 190);
        case STEAM: return rng_u8(75, 145);
        case WATER:
        case LAVA:
        case ACID:
        case OIL: return LIQUID_SETTLE_FRAMES;
        default: return 0;
    }
}

// ===========================================================================
// Boucle principale et chunks
// ===========================================================================

void FallingSandGrid::_process(double p_delta) {
    (void)p_delta;
    begin_frame();
    run_simulation();
    render_grid();
    update_stats();
    queue_redraw();
}

void FallingSandGrid::begin_frame() {
    active_chunk_count = 0;
    dirty_cell_count = 0;
    for (std::vector<int> &list : pass_lists) {
        list.clear();
    }
    debug_chunk_rects.clear();
    debug_dirty_rects.clear();

    for (int chunk_index = 0; chunk_index < NUM_CHUNKS; ++chunk_index) {
        Chunk &chunk = chunks[chunk_index];

        chunk.min_x = chunk.w_min_x.load(std::memory_order_relaxed);
        chunk.min_y = chunk.w_min_y.load(std::memory_order_relaxed);
        chunk.max_x = chunk.w_max_x.load(std::memory_order_relaxed);
        chunk.max_y = chunk.w_max_y.load(std::memory_order_relaxed);
        chunk.reset_working();

        if (!chunk.active()) {
            continue;
        }

        for (int y = chunk.min_y; y <= chunk.max_y; ++y) {
            uint8_t *row = grid.data() + static_cast<size_t>(y) * WORLD_WIDTH;
            for (int x = chunk.min_x; x <= chunk.max_x; ++x) {
                row[x] &= TYPE_MASK;
            }
        }

        const int chunk_x = chunk_index % CHUNKS_X;
        const int chunk_y = chunk_index / CHUNKS_X;
        pass_lists[(CHUNKS_Y - 1 - chunk_y) * 2 + (chunk_x & 1)].push_back(chunk_index);

        ++active_chunk_count;
        dirty_cell_count += static_cast<int64_t>(chunk.max_x - chunk.min_x + 1) *
                            (chunk.max_y - chunk.min_y + 1);

        if (debug_overlay) {
            debug_chunk_rects.emplace_back(
                    chunk_x * CHUNK_SIZE, chunk_y * CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE);
            debug_dirty_rects.emplace_back(
                    chunk.min_x, chunk.min_y,
                    chunk.max_x - chunk.min_x + 1,
                    chunk.max_y - chunk.min_y + 1);
        }
    }
}

void FallingSandGrid::run_simulation() {
    WorkerThreadPool *thread_pool = WorkerThreadPool::get_singleton();

    for (int pass = 0; pass < NUM_PASSES; ++pass) {
        if (pass_lists[pass].empty()) {
            continue;
        }
        current_pass = pass;

        if (use_threads && pass_lists[pass].size() > 1 && thread_pool != nullptr) {
            const int64_t group_id = thread_pool->add_group_task(
                    Callable(this, "_simulate_chunk"),
                    static_cast<int32_t>(pass_lists[pass].size()),
                    -1, true, "FallingSand");
            thread_pool->wait_for_group_task_completion(group_id);
        } else {
            for (int list_index = 0; list_index < static_cast<int>(pass_lists[pass].size()); ++list_index) {
                _simulate_chunk(list_index);
            }
        }
    }
}

void FallingSandGrid::_simulate_chunk(int list_index) {
    const std::vector<int> &list = pass_lists[current_pass];
    if (list_index < 0 || list_index >= static_cast<int>(list.size())) {
        return;
    }
    simulate_chunk_cells(chunks[list[list_index]]);
}

void FallingSandGrid::simulate_chunk_cells(const Chunk &chunk) {
    for (int y = chunk.max_y; y >= chunk.min_y; --y) {
        const bool left_to_right = rng_bool();
        const int x_start = left_to_right ? chunk.min_x : chunk.max_x;
        const int x_end = left_to_right ? chunk.max_x + 1 : chunk.min_x - 1;
        const int step = left_to_right ? 1 : -1;

        for (int x = x_start; x != x_end; x += step) {
            const uint8_t encoded = grid[get_index(x, y)];
            if ((encoded & UPDATED_BIT) != 0) {
                continue;
            }

            const Particle type = static_cast<Particle>(encoded & TYPE_MASK);
            switch (type) {
                case SAND:
                    move_sand(x, y);
                    break;
                case WATER:
                case LAVA:
                case ACID:
                case OIL:
                    move_liquid(x, y, type);
                    break;
                case SMOKE:
                case STEAM:
                    move_smoke_or_steam(x, y, type);
                    break;
                case FIRE:
                    move_fire(x, y);
                    break;
                default:
                    break;
            }
        }
    }
}

// ===========================================================================
// Ecriture, deplacement et reveil
// ===========================================================================

void FallingSandGrid::set_cell(int x, int y, Particle type, uint8_t data, bool updated) {
    if (!in_world(x, y)) {
        return;
    }

    const int index = get_index(x, y);
    grid[index] = static_cast<uint8_t>(type);
    if (updated && type != EMPTY) {
        grid[index] |= UPDATED_BIT;
    }
    cell_data[index] = (type == EMPTY) ? 0 : data;
    mark_dirty(x, y);
}

void FallingSandGrid::move_cell(int x1, int y1, int x2, int y2) {
    const int source_index = get_index(x1, y1);
    const int target_index = get_index(x2, y2);

    const Particle source_type = static_cast<Particle>(grid[source_index] & TYPE_MASK);
    const Particle target_type = static_cast<Particle>(grid[target_index] & TYPE_MASK);
    const uint8_t source_data = cell_data[source_index];
    const uint8_t target_data = cell_data[target_index];

    grid[target_index] = static_cast<uint8_t>(source_type) | UPDATED_BIT;
    cell_data[target_index] = source_data;

    if (target_type == EMPTY) {
        grid[source_index] = static_cast<uint8_t>(EMPTY);
        cell_data[source_index] = 0;
    } else {
        grid[source_index] = static_cast<uint8_t>(target_type) | UPDATED_BIT;
        cell_data[source_index] = target_data;
    }

    if (is_liquid(source_type)) {
        cell_data[target_index] = LIQUID_SETTLE_FRAMES;
    }
    if (is_liquid(target_type)) {
        cell_data[source_index] = LIQUID_SETTLE_FRAMES;
    }

    mark_dirty(x1, y1);
    mark_dirty(x2, y2);
}

void FallingSandGrid::keep_liquid_awake(int x, int y) {
    const int index = get_index(x, y);
    if (cell_data[index] > 0) {
        --cell_data[index];
        mark_dirty(x, y);
    }
}

void FallingSandGrid::mark_dirty(int x, int y, int radius_x, int radius_y) {
    const int x0 = std::max(x - radius_x, 0);
    const int x1 = std::min(x + radius_x, WORLD_WIDTH - 1);
    const int y0 = std::max(y - radius_y, 0);
    const int y1 = std::min(y + radius_y, WORLD_HEIGHT - 1);

    const int chunk_x0 = x0 / CHUNK_SIZE;
    const int chunk_x1 = x1 / CHUNK_SIZE;
    const int chunk_y0 = y0 / CHUNK_SIZE;
    const int chunk_y1 = y1 / CHUNK_SIZE;

    for (int chunk_y = chunk_y0; chunk_y <= chunk_y1; ++chunk_y) {
        for (int chunk_x = chunk_x0; chunk_x <= chunk_x1; ++chunk_x) {
            Chunk &chunk = chunks[chunk_y * CHUNKS_X + chunk_x];
            atomic_fetch_min(chunk.w_min_x, std::max(x0, chunk_x * CHUNK_SIZE));
            atomic_fetch_min(chunk.w_min_y, std::max(y0, chunk_y * CHUNK_SIZE));
            atomic_fetch_max(chunk.w_max_x, std::min(x1, (chunk_x + 1) * CHUNK_SIZE - 1));
            atomic_fetch_max(chunk.w_max_y, std::min(y1, (chunk_y + 1) * CHUNK_SIZE - 1));
        }
    }
}

// ===========================================================================
// Sable et liquides
// ===========================================================================

void FallingSandGrid::move_sand(int x, int y) {
    if (y + 1 >= WORLD_HEIGHT) {
        return;
    }

    if (can_sink_into(SAND, cell_type(x, y + 1))) {
        move_cell(x, y, x, y + 1);
        return;
    }

    const int first_direction = rng_bool() ? -1 : 1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int direction = (attempt == 0) ? first_direction : -first_direction;
        const int next_x = x + direction;
        if (!in_world(next_x, y + 1)) {
            continue;
        }

        const Particle diagonal = cell_type(next_x, y + 1);
        const Particle side = cell_type(next_x, y);
        if (can_sink_into(SAND, diagonal) && !is_solid(side) && side != SAND) {
            move_cell(x, y, next_x, y + 1);
            return;
        }
    }
}

FallingSandGrid::FlowCandidate FallingSandGrid::scan_liquid_side(
        int x, int y, Particle type, int direction, int max_distance) const {
    FlowCandidate candidate;
    candidate.x = x;

    for (int distance = 1; distance <= max_distance; ++distance) {
        const int target_x = x + direction * distance;
        if (target_x < 0 || target_x >= WORLD_WIDTH) {
            break;
        }

        const Particle target = cell_type(target_x, y);
        if (target == EMPTY) {
            candidate.x = target_x;
            candidate.distance = distance;
        } else if (distance == 1 && can_sink_into(type, target)) {
            candidate.x = target_x;
            candidate.distance = 1;
            break;
        } else {
            break;
        }

        if (y + 1 < WORLD_HEIGHT && can_sink_into(type, cell_type(target_x, y + 1))) {
            candidate.has_drop = true;
            break;
        }
    }

    return candidate;
}

void FallingSandGrid::move_liquid(int x, int y, Particle type) {
    if (type == WATER && process_water_reactions(x, y)) {
        return;
    }
    if (type == LAVA && process_lava_reactions(x, y)) {
        return;
    }
    if (type == ACID && process_acid_reactions(x, y)) {
        return;
    }

    if (y + 1 < WORLD_HEIGHT && can_sink_into(type, cell_type(x, y + 1))) {
        move_cell(x, y, x, y + 1);
        return;
    }

    if (y + 1 < WORLD_HEIGHT) {
        const int first_direction = rng_bool() ? -1 : 1;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int direction = (attempt == 0) ? first_direction : -first_direction;
            const int next_x = x + direction;
            if (!in_world(next_x, y + 1)) {
                continue;
            }

            const Particle side = cell_type(next_x, y);
            const Particle diagonal = cell_type(next_x, y + 1);
            if (can_sink_into(type, diagonal) && !is_solid(side) && side != SAND) {
                move_cell(x, y, next_x, y + 1);
                return;
            }
        }
    }

    const int dispersion = liquid_dispersion(type);
    const FlowCandidate left = scan_liquid_side(x, y, type, -1, dispersion);
    const FlowCandidate right = scan_liquid_side(x, y, type, 1, dispersion);

    const FlowCandidate *chosen = nullptr;
    if (left.has_drop != right.has_drop) {
        chosen = left.has_drop ? &left : &right;
    } else if (left.has_drop && right.has_drop) {
        if (left.distance == right.distance) {
            chosen = rng_bool() ? &left : &right;
        } else {
            chosen = (left.distance < right.distance) ? &left : &right;
        }
    } else if (left.distance != right.distance) {
        chosen = (left.distance > right.distance) ? &left : &right;
    } else if (left.distance > 0) {
        chosen = rng_bool() ? &left : &right;
    }

    if (chosen != nullptr && chosen->distance > 0) {
        move_cell(x, y, chosen->x, y);
        return;
    }

    // Une cellule liquide immobile continue d'etre testee quelques frames.
    // Cela evite qu'une flaque se fige prematurement sur les frontieres des
    // dirty rects, tout en permettant au chunk de s'endormir ensuite.
    keep_liquid_awake(x, y);
}

// ===========================================================================
// Gaz, vapeur et feu
// ===========================================================================

bool FallingSandGrid::try_move_gas(int x, int y, Particle type, int horizontal_dispersion) {
    if (y > 0 && can_gas_rise_into(type, cell_type(x, y - 1))) {
        move_cell(x, y, x, y - 1);
        return true;
    }

    if (y > 0) {
        const int first_direction = rng_bool() ? -1 : 1;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const int direction = (attempt == 0) ? first_direction : -first_direction;
            const int next_x = x + direction;
            if (!in_world(next_x, y - 1)) {
                continue;
            }
            if (can_gas_rise_into(type, cell_type(next_x, y - 1)) &&
                !is_solid(cell_type(next_x, y))) {
                move_cell(x, y, next_x, y - 1);
                return true;
            }
        }
    }

    int available_left = 0;
    int available_right = 0;
    for (int distance = 1; distance <= horizontal_dispersion; ++distance) {
        const int target_x = x - distance;
        if (target_x < 0 || !can_gas_rise_into(type, cell_type(target_x, y))) {
            break;
        }
        available_left = distance;
    }
    for (int distance = 1; distance <= horizontal_dispersion; ++distance) {
        const int target_x = x + distance;
        if (target_x >= WORLD_WIDTH || !can_gas_rise_into(type, cell_type(target_x, y))) {
            break;
        }
        available_right = distance;
    }

    if (available_left == 0 && available_right == 0) {
        return false;
    }

    int target_x = x;
    if (available_left == available_right) {
        target_x += rng_bool() ? available_right : -available_left;
    } else if (available_left > available_right) {
        target_x -= available_left;
    } else {
        target_x += available_right;
    }

    move_cell(x, y, target_x, y);
    return true;
}

void FallingSandGrid::move_smoke_or_steam(int x, int y, Particle type) {
    const int index = get_index(x, y);
    uint8_t life = cell_data[index];
    if (life == 0) {
        life = initial_cell_data(type);
    }

    if (life <= 1) {
        if (type == STEAM && rng_chance(1, 3)) {
            set_cell(x, y, WATER, LIQUID_SETTLE_FRAMES, true);
        } else {
            set_cell(x, y, EMPTY, 0, false);
        }
        return;
    }

    cell_data[index] = static_cast<uint8_t>(life - 1);
    if (!try_move_gas(x, y, type, GAS_DISPERSION)) {
        // Un gaz bloque doit rester actif pour vieillir, puis mourir.
        mark_dirty(x, y);
    }
}

void FallingSandGrid::move_fire(int x, int y) {
    if (process_fire_reactions(x, y)) {
        return;
    }

    const int index = get_index(x, y);
    uint8_t life = cell_data[index];
    if (life == 0) {
        life = initial_cell_data(FIRE);
    }

    if (life <= 1) {
        set_cell(x, y, SMOKE, initial_cell_data(SMOKE), true);
        return;
    }

    cell_data[index] = static_cast<uint8_t>(life - 1);
    if (!try_move_gas(x, y, FIRE, 2)) {
        mark_dirty(x, y);
    }
}

// ===========================================================================
// Reactions
// ===========================================================================

bool FallingSandGrid::process_water_reactions(int x, int y) {
    bool extinguished_fire = false;

    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            if (offset_x == 0 && offset_y == 0) {
                continue;
            }
            const int neighbor_x = x + offset_x;
            const int neighbor_y = y + offset_y;
            if (!in_world(neighbor_x, neighbor_y)) {
                continue;
            }
            if (cell_type(neighbor_x, neighbor_y) == FIRE) {
                set_cell(neighbor_x, neighbor_y, STEAM, initial_cell_data(STEAM), true);
                extinguished_fire = true;
            }
        }
    }

    if (extinguished_fire) {
        cell_data[get_index(x, y)] = LIQUID_SETTLE_FRAMES;
        mark_dirty(x, y);
    }
    return extinguished_fire;
}

bool FallingSandGrid::process_fire_reactions(int x, int y) {
    // L'eau eteint immediatement le feu : la cellule de feu devient vapeur.
    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            if (offset_x == 0 && offset_y == 0) {
                continue;
            }
            const int neighbor_x = x + offset_x;
            const int neighbor_y = y + offset_y;
            if (in_world(neighbor_x, neighbor_y) && cell_type(neighbor_x, neighbor_y) == WATER) {
                set_cell(x, y, STEAM, initial_cell_data(STEAM), true);
                return true;
            }
        }
    }

    // Le feu enflamme le bois lentement et l'huile beaucoup plus vite.
    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            if (offset_x == 0 && offset_y == 0) {
                continue;
            }
            const int neighbor_x = x + offset_x;
            const int neighbor_y = y + offset_y;
            if (!in_world(neighbor_x, neighbor_y)) {
                continue;
            }

            const Particle neighbor = cell_type(neighbor_x, neighbor_y);
            const bool ignite_wood = neighbor == WOOD && rng_chance(1, 42);
            const bool ignite_oil = neighbor == OIL && rng_chance(1, 7);
            if (ignite_wood || ignite_oil) {
                set_cell(neighbor_x, neighbor_y, FIRE, initial_cell_data(FIRE), true);
            }
        }
    }

    return false;
}

bool FallingSandGrid::process_lava_reactions(int x, int y) {
    bool reacted = false;

    for (int offset_y = -1; offset_y <= 1; ++offset_y) {
        for (int offset_x = -1; offset_x <= 1; ++offset_x) {
            if (offset_x == 0 && offset_y == 0) {
                continue;
            }
            const int neighbor_x = x + offset_x;
            const int neighbor_y = y + offset_y;
            if (!in_world(neighbor_x, neighbor_y)) {
                continue;
            }

            const Particle neighbor = cell_type(neighbor_x, neighbor_y);
            if (neighbor == WATER) {
                // La lave mineralise l'eau au contact et libere de la vapeur.
                set_cell(neighbor_x, neighbor_y, ROCK, 0, true);
                spawn_steam_near(neighbor_x, neighbor_y);
                reacted = true;
            } else if (is_flammable(neighbor) && rng_chance(1, 10)) {
                set_cell(neighbor_x, neighbor_y, FIRE, initial_cell_data(FIRE), true);
                reacted = true;
            }
        }
    }

    if (reacted) {
        cell_data[get_index(x, y)] = LIQUID_SETTLE_FRAMES;
        mark_dirty(x, y);
    }
    return reacted;
}

bool FallingSandGrid::process_acid_reactions(int x, int y) {
    const int start = static_cast<int>(rng_next() % 8u);
    static const int OFFSETS[8][2] = {
        {-1, -1}, {0, -1}, {1, -1},
        {-1, 0},            {1, 0},
        {-1, 1},  {0, 1},   {1, 1}
    };

    for (int step = 0; step < 8; ++step) {
        const int offset_index = (start + step) % 8;
        const int neighbor_x = x + OFFSETS[offset_index][0];
        const int neighbor_y = y + OFFSETS[offset_index][1];
        if (!in_world(neighbor_x, neighbor_y)) {
            continue;
        }

        const Particle neighbor = cell_type(neighbor_x, neighbor_y);
        bool dissolve = false;
        if (neighbor == WOOD) {
            dissolve = rng_chance(1, 10);
        } else if (neighbor == SAND) {
            dissolve = rng_chance(1, 18);
        } else if (neighbor == ROCK) {
            dissolve = rng_chance(1, 55);
        }

        if (dissolve) {
            set_cell(neighbor_x, neighbor_y, EMPTY, 0, false);
            if (rng_chance(1, 12)) {
                set_cell(x, y, EMPTY, 0, false);
            } else {
                cell_data[get_index(x, y)] = LIQUID_SETTLE_FRAMES;
                mark_dirty(x, y);
            }
            return true;
        }
    }

    return false;
}

void FallingSandGrid::spawn_steam_near(int x, int y) {
    static const int OFFSETS[8][2] = {
        {0, -1}, {-1, -1}, {1, -1},
        {-1, 0}, {1, 0},
        {0, -2}, {-2, -1}, {2, -1}
    };

    for (const auto &offset : OFFSETS) {
        const int target_x = x + offset[0];
        const int target_y = y + offset[1];
        if (in_world(target_x, target_y) && cell_type(target_x, target_y) == EMPTY) {
            set_cell(target_x, target_y, STEAM, initial_cell_data(STEAM), true);
            return;
        }
    }
}

// ===========================================================================
// Rendu et interface
// ===========================================================================

void FallingSandGrid::render_grid() {
    std::memcpy(pixel_bytes.ptrw(), grid.data(), grid.size());
    image->set_data(WORLD_WIDTH, WORLD_HEIGHT, false, Image::FORMAT_R8, pixel_bytes);
    texture->update(image);
}

void FallingSandGrid::_draw() {
    const Vector2 mouse = get_local_mouse_position();
    if (mouse.y < static_cast<real_t>(WORLD_HEIGHT * CELL_SIZE)) {
        draw_arc(mouse, static_cast<real_t>(brush_radius * CELL_SIZE),
                 0.0f, 6.2831853f, 48,
                 Color(1.0f, 1.0f, 1.0f, 0.62f), 1.0f);
    }

    if (!debug_overlay) {
        return;
    }

    for (const Rect2i &rect : debug_chunk_rects) {
        draw_rect(Rect2(
                          static_cast<real_t>(rect.position.x * CELL_SIZE),
                          static_cast<real_t>(rect.position.y * CELL_SIZE),
                          static_cast<real_t>(rect.size.x * CELL_SIZE),
                          static_cast<real_t>(rect.size.y * CELL_SIZE)),
                  Color(1.0f, 1.0f, 0.0f, 0.35f), false, 1.0f);
    }
    for (const Rect2i &rect : debug_dirty_rects) {
        draw_rect(Rect2(
                          static_cast<real_t>(rect.position.x * CELL_SIZE),
                          static_cast<real_t>(rect.position.y * CELL_SIZE),
                          static_cast<real_t>(rect.size.x * CELL_SIZE),
                          static_cast<real_t>(rect.size.y * CELL_SIZE)),
                  Color(1.0f, 0.15f, 0.15f, 0.90f), false, 1.0f);
    }
}

void FallingSandGrid::setup_ui() {
    ui_panel = memnew(Panel);
    ui_panel->set_position(Vector2(0, static_cast<real_t>(WORLD_HEIGHT * CELL_SIZE)));
    ui_panel->set_size(Vector2(static_cast<real_t>(WORLD_WIDTH * CELL_SIZE),
                               static_cast<real_t>(UI_HEIGHT)));
    add_child(ui_panel);

    constexpr int columns = 6;
    constexpr int button_width = 112;
    constexpr int button_height = 34;
    constexpr int horizontal_gap = 8;
    constexpr int vertical_gap = 7;
    constexpr int start_x = 12;
    constexpr int start_y = 10;

    for (int index = 0; index < PARTICLE_INFO_COUNT; ++index) {
        const int column = index % columns;
        const int row = index / columns;

        Button *button = memnew(Button);
        button->set_text(PARTICLE_INFOS[index].name);
        button->set_position(Vector2(
                static_cast<real_t>(start_x + column * (button_width + horizontal_gap)),
                static_cast<real_t>(start_y + row * (button_height + vertical_gap))));
        button->set_size(Vector2(static_cast<real_t>(button_width),
                                 static_cast<real_t>(button_height)));
        button->connect("pressed",
                        Callable(this, "on_particle_button_pressed")
                                .bind(static_cast<int>(PARTICLE_INFOS[index].type)));

        ui_panel->add_child(button);
        ui_buttons[PARTICLE_INFOS[index].type] = button;
    }

    stats_label = memnew(Label);
    stats_label->set_position(Vector2(748.0f, 8.0f));
    stats_label->set_size(Vector2(
            static_cast<real_t>(WORLD_WIDTH * CELL_SIZE - 760),
            static_cast<real_t>(UI_HEIGHT - 12)));
    ui_panel->add_child(stats_label);

    update_button_styles();
}

void FallingSandGrid::update_button_styles() {
    for (const ParticleInfo &info : PARTICLE_INFOS) {
        Button *button = ui_buttons[info.type];
        const bool selected = info.type == selected_particle;
        button->add_theme_stylebox_override("normal", make_button_style(info.color, selected, 0.0f));
        button->add_theme_stylebox_override("hover", make_button_style(info.color, selected, 0.13f));
        button->add_theme_stylebox_override("pressed", make_button_style(info.color, selected, -0.12f));
    }
}

void FallingSandGrid::update_stats() {
    if (stats_label == nullptr) {
        return;
    }

    ++frame_counter;
    if (frame_counter % 10 != 0) {
        return;
    }

    const double fps = Engine::get_singleton()->get_frames_per_second();

    String text;
    text += String("FPS: ") + String::num_int64(static_cast<int64_t>(std::lround(fps)));
    text += String("  |  Chunks: ") + String::num_int64(active_chunk_count) +
            String("/") + String::num_int64(NUM_CHUNKS);
    text += String("  |  Cellules: ") + String::num_int64(dirty_cell_count);
    text += String("  |  Threads: ") + String(use_threads ? "ON" : "OFF");
    text += String("\nMateriau: ") + String(particle_name(selected_particle));
    text += String("  |  Pinceau: ") + String::num_int64(brush_radius);
    text += String("  |  [D] chunks  [T] threads  [C] effacer  [Molette] taille");
    stats_label->set_text(text);
}

void FallingSandGrid::on_particle_button_pressed(int particle_type) {
    selected_particle = static_cast<Particle>(particle_type);
    update_button_styles();
}

// ===========================================================================
// Interaction
// ===========================================================================

void FallingSandGrid::_input(const Ref<InputEvent> &event) {
    Ref<InputEventMouseButton> mouse_button = event;
    if (mouse_button.is_valid()) {
        if (mouse_button->get_button_index() == MOUSE_BUTTON_LEFT) {
            if (mouse_button->is_pressed()) {
                painting = true;
                const Vector2i cell = mouse_to_cell();
                paint_at(cell);
                last_paint_cell = cell;
            } else {
                painting = false;
            }
        } else if (mouse_button->is_pressed() &&
                   mouse_button->get_button_index() == MOUSE_BUTTON_WHEEL_UP) {
            brush_radius = std::min(brush_radius + 1, 40);
        } else if (mouse_button->is_pressed() &&
                   mouse_button->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) {
            brush_radius = std::max(brush_radius - 1, 1);
        }
        return;
    }

    Ref<InputEventMouseMotion> mouse_motion = event;
    if (mouse_motion.is_valid()) {
        if (painting && mouse_motion->get_button_mask().has_flag(MOUSE_BUTTON_MASK_LEFT)) {
            const Vector2i cell = mouse_to_cell();
            paint_line(last_paint_cell, cell);
            last_paint_cell = cell;
        }
        return;
    }

    Ref<InputEventKey> key = event;
    if (key.is_valid() && key->is_pressed() && !key->is_echo()) {
        const Key keycode = key->get_keycode();
        if (keycode == KEY_D) {
            debug_overlay = !debug_overlay;
        } else if (keycode == KEY_T) {
            use_threads = !use_threads;
        } else if (keycode == KEY_C) {
            clear_world();
        }
    }
}

Vector2i FallingSandGrid::mouse_to_cell() const {
    const Vector2 mouse = get_local_mouse_position();
    return Vector2i(
            static_cast<int>(std::floor(mouse.x / static_cast<float>(CELL_SIZE))),
            static_cast<int>(std::floor(mouse.y / static_cast<float>(CELL_SIZE))));
}

void FallingSandGrid::paint_at(Vector2i cell) {
    const int radius_squared = brush_radius * brush_radius;

    for (int offset_y = -brush_radius; offset_y <= brush_radius; ++offset_y) {
        for (int offset_x = -brush_radius; offset_x <= brush_radius; ++offset_x) {
            if (offset_x * offset_x + offset_y * offset_y > radius_squared) {
                continue;
            }

            const int x = cell.x + offset_x;
            const int y = cell.y + offset_y;
            if (!in_world(x, y)) {
                continue;
            }

            const int index = get_index(x, y);
            grid[index] = static_cast<uint8_t>(selected_particle);
            cell_data[index] = initial_cell_data(selected_particle);
            mark_dirty(x, y);
        }
    }
}

void FallingSandGrid::paint_line(Vector2i from, Vector2i to) {
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
    if (steps == 0) {
        paint_at(to);
        return;
    }

    for (int step = 0; step <= steps; ++step) {
        const float ratio = static_cast<float>(step) / static_cast<float>(steps);
        const Vector2i point(
                static_cast<int>(std::lround(from.x + (to.x - from.x) * ratio)),
                static_cast<int>(std::lround(from.y + (to.y - from.y) * ratio)));
        paint_at(point);
    }
}

void FallingSandGrid::clear_world() {
    std::fill(grid.begin(), grid.end(), static_cast<uint8_t>(EMPTY));
    std::fill(cell_data.begin(), cell_data.end(), 0);

    for (int chunk_index = 0; chunk_index < NUM_CHUNKS; ++chunk_index) {
        chunks[chunk_index].reset_working();
        chunks[chunk_index].min_x = 0;
        chunks[chunk_index].min_y = 0;
        chunks[chunk_index].max_x = -1;
        chunks[chunk_index].max_y = -1;
    }
}
