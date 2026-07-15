#include "falling_sand_grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/callable.hpp>

using namespace godot;

// ---------------------------------------------------------------------------
// Garde-fous : le damier 2x2 n'est sûr que si une particule ne peut pas
// parcourir plus d'un demi-chunk en une frame.
// ---------------------------------------------------------------------------
static_assert(FallingSandGrid::WORLD_WIDTH % FallingSandGrid::CHUNK_SIZE == 0,
              "WORLD_WIDTH doit etre un multiple de CHUNK_SIZE");
static_assert(FallingSandGrid::WORLD_HEIGHT % FallingSandGrid::CHUNK_SIZE == 0,
              "WORLD_HEIGHT doit etre un multiple de CHUNK_SIZE");
static_assert(FallingSandGrid::WATER_DISPERSION * 2 + 2 < FallingSandGrid::CHUNK_SIZE,
              "WATER_DISPERSION trop grand pour la securite du damier multithread");

namespace {

// ---------------------------------------------------------------------------
// RNG xorshift32 par thread : UtilityFunctions::randf() est beaucoup trop
// lent pour etre appele des dizaines de milliers de fois par frame, et n'est
// pas prevu pour etre appele depuis les threads du WorkerThreadPool.
// Chaque thread se seede tout seul avec l'adresse de sa variable thread_local.
// ---------------------------------------------------------------------------
inline uint32_t rng_next() {
    thread_local uint32_t state = 0;
    if (state == 0) {
        const uint64_t p = (uint64_t)(uintptr_t)&state;
        state = (uint32_t)(p ^ (p >> 32)) * 2654435761u;
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

inline bool rng_bool() { return (rng_next() & 1u) != 0u; }

// min/max atomiques via boucle compare-exchange (C++ n'a pas fetch_min).
inline void atomic_fetch_min(std::atomic<int> &a, int v) {
    int cur = a.load(std::memory_order_relaxed);
    while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

inline void atomic_fetch_max(std::atomic<int> &a, int v) {
    int cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

// ---------------------------------------------------------------------------
// Shader de rendu : la texture R8 contient directement les octets de la
// grille (type + UPDATED_BIT). Le shader retire le bit 7 (% 128) puis mappe
// type -> couleur, avec un peu de variation par cellule et une eau animee.
// ---------------------------------------------------------------------------
const char *SAND_SHADER = R"SHADER(
shader_type canvas_item;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

void fragment() {
    ivec2 ts = textureSize(TEXTURE, 0);
    vec2 cell = floor(UV * vec2(ts));

    int t = int(texture(TEXTURE, UV).r * 255.0 + 0.5);
    t = clamp(t % 128, 0, 3);

    vec3 col;
    if (t == 0) {
        // vide
        col = vec3(0.04, 0.04, 0.06);
    } else if (t == 1) {
        // sable : legere variation de teinte par cellule
        col = vec3(0.93, 0.63, 0.36) * (0.85 + 0.15 * hash(cell));
    } else if (t == 2) {
        // eau : shimmer anime gratuit sur GPU
        float w = 0.85 + 0.15 * sin(TIME * 2.5 + cell.x * 0.15 + cell.y * 0.3 + hash(cell) * 6.2831);
        col = vec3(0.15, 0.50, 0.95) * w;
    } else {
        // roche : bruit statique
        col = vec3(0.42, 0.42, 0.45) * (0.80 + 0.20 * hash(cell * 1.7));
    }
    COLOR = vec4(col, 1.0);
}
)SHADER";

// Style de bouton (normal / survole / selectionne).
Ref<StyleBoxFlat> make_button_style(const Color &base, bool selected, float lighten) {
    Ref<StyleBoxFlat> style;
    style.instantiate();
    style->set_bg_color(selected ? base.lightened(0.3f + lighten) : base.lightened(lighten));
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

const ParticleInfo PARTICLE_INFOS[4] = {
    { SAND, "Sable", Color(0.96f, 0.64f, 0.38f) },
    { WATER, "Eau", Color(0.0f, 0.75f, 1.0f) },
    { ROCK, "Roche", Color(0.41f, 0.41f, 0.41f) },
    { EMPTY, "Effacer", Color(0.15f, 0.15f, 0.15f) },
};

} // namespace

// ===========================================================================
//  Cycle de vie
// ===========================================================================

FallingSandGrid::FallingSandGrid() {
    grid.assign((size_t)WORLD_WIDTH * WORLD_HEIGHT, (uint8_t)EMPTY);
    chunks = std::make_unique<Chunk[]>(NUM_CHUNKS);
}

FallingSandGrid::~FallingSandGrid() {
}

void FallingSandGrid::_bind_methods() {
    ClassDB::bind_method(D_METHOD("on_particle_button_pressed", "particle_type"),
                         &FallingSandGrid::on_particle_button_pressed);
    // Bindee pour pouvoir etre appelee par le WorkerThreadPool via Callable.
    ClassDB::bind_method(D_METHOD("_simulate_chunk", "list_index"),
                         &FallingSandGrid::_simulate_chunk);
}

void FallingSandGrid::_ready() {
    DisplayServer::get_singleton()->window_set_size(
            Vector2i(WORLD_WIDTH * CELL_SIZE, WORLD_HEIGHT * CELL_SIZE + UI_HEIGHT));

    // Buffer de pixels persistant : un seul memcpy par frame, zero set_pixel.
    pixel_bytes.resize((int64_t)WORLD_WIDTH * WORLD_HEIGHT);
    image = Image::create_from_data(WORLD_WIDTH, WORLD_HEIGHT, false, Image::FORMAT_R8, pixel_bytes);
    texture = ImageTexture::create_from_image(image);

    sprite = memnew(Sprite2D);
    sprite->set_texture(texture);
    sprite->set_centered(false);
    sprite->set_scale(Vector2((real_t)CELL_SIZE, (real_t)CELL_SIZE));
    sprite->set_texture_filter(CanvasItem::TEXTURE_FILTER_NEAREST);
    // Le sprite se dessine SOUS le _draw() du parent, pour que l'overlay de
    // debug (dirty rects) et le curseur de pinceau restent visibles.
    sprite->set_draw_behind_parent(true);
    add_child(sprite);

    setup_material();
    setup_ui();
    render_grid();
}

void FallingSandGrid::setup_material() {
    Ref<Shader> shader;
    shader.instantiate();
    shader->set_code(SAND_SHADER);

    Ref<ShaderMaterial> material;
    material.instantiate();
    material->set_shader(shader);
    sprite->set_material(material);
}

// ===========================================================================
//  Boucle principale
// ===========================================================================

void FallingSandGrid::_process(double p_delta) {
    begin_frame();
    run_simulation();
    render_grid();
    update_stats();
    queue_redraw(); // curseur de pinceau + overlay de debug
}

// Bascule les working rects (remplis a la frame precedente) en rects
// courants, efface les UPDATED_BIT dans ces zones, et construit les 4
// listes de chunks actifs (une par passe du damier).
// Invariant : toute cellule dont le bit est pose se trouve forcement dans le
// working rect d'un chunk (move_cell marque toujours la destination dirty),
// donc ce clear borne aux rects suffit — jamais de passe plein ecran.
void FallingSandGrid::begin_frame() {
    active_chunk_count = 0;
    dirty_cell_count = 0;
    for (int p = 0; p < 4; p++) {
        pass_lists[p].clear();
    }
    debug_chunk_rects.clear();
    debug_dirty_rects.clear();

    for (int ci = 0; ci < NUM_CHUNKS; ci++) {
        Chunk &c = chunks[ci];

        c.min_x = c.w_min_x.load(std::memory_order_relaxed);
        c.min_y = c.w_min_y.load(std::memory_order_relaxed);
        c.max_x = c.w_max_x.load(std::memory_order_relaxed);
        c.max_y = c.w_max_y.load(std::memory_order_relaxed);
        c.reset_working();

        if (!c.active()) {
            continue; // chunk endormi : cout zero.
        }

        // Clear des bits "deja simule" poses a la frame precedente.
        for (int y = c.min_y; y <= c.max_y; y++) {
            uint8_t *row = grid.data() + (size_t)y * WORLD_WIDTH;
            for (int x = c.min_x; x <= c.max_x; x++) {
                row[x] &= TYPE_MASK;
            }
        }

        const int cx = ci % CHUNKS_X;
        const int cy = ci / CHUNKS_X;
        pass_lists[(cx & 1) | ((cy & 1) << 1)].push_back(ci);

        active_chunk_count++;
        dirty_cell_count += (int64_t)(c.max_x - c.min_x + 1) * (c.max_y - c.min_y + 1);

        if (debug_overlay) {
            debug_chunk_rects.push_back(Rect2i(cx * CHUNK_SIZE, cy * CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE));
            debug_dirty_rects.push_back(Rect2i(c.min_x, c.min_y, c.max_x - c.min_x + 1, c.max_y - c.min_y + 1));
        }
    }
}

// 4 passes en damier 2x2. A l'interieur d'une passe, tous les chunks sont
// simules en parallele : ils sont separes d'au moins un chunk complet
// (64 px), et une particule bouge d'au plus WATER_DISPERSION cellules,
// donc aucune ecriture concurrente possible sur la grille.
void FallingSandGrid::run_simulation() {
    WorkerThreadPool *wtp = WorkerThreadPool::get_singleton();

    for (int p = 0; p < 4; p++) {
        if (pass_lists[p].empty()) {
            continue;
        }
        current_pass = p;

        if (use_threads && pass_lists[p].size() > 1 && wtp != nullptr) {
            const int64_t group = wtp->add_group_task(
                    Callable(this, "_simulate_chunk"),
                    (int32_t)pass_lists[p].size(),
                    -1, true, "FallingSand");
            wtp->wait_for_group_task_completion(group);
        } else {
            for (int i = 0; i < (int)pass_lists[p].size(); i++) {
                _simulate_chunk(i);
            }
        }
    }
}

void FallingSandGrid::_simulate_chunk(int list_index) {
    const std::vector<int> &list = pass_lists[current_pass];
    if (list_index < 0 || list_index >= (int)list.size()) {
        return;
    }
    simulate_chunk_cells(chunks[list[list_index]]);
}

void FallingSandGrid::simulate_chunk_cells(const Chunk &c) {
    // Bas -> haut, direction horizontale aleatoire par ligne (evite le biais).
    for (int y = c.max_y; y >= c.min_y; y--) {
        const bool ltr = rng_bool();
        const int x_start = ltr ? c.min_x : c.max_x;
        const int x_end = ltr ? c.max_x + 1 : c.min_x - 1;
        const int step = ltr ? 1 : -1;

        for (int x = x_start; x != x_end; x += step) {
            const uint8_t cell = grid[get_index(x, y)];
            if (cell & UPDATED_BIT) {
                continue; // deja simulee cette frame (arrivee d'un autre chunk).
            }
            if (cell == SAND) {
                move_sand(x, y);
            } else if (cell == WATER) {
                move_water(x, y);
            }
            // EMPTY et ROCK : rien a faire.
        }
    }
}

// ===========================================================================
//  Regles de particules
// ===========================================================================

void FallingSandGrid::move_sand(int x, int y) {
    if (y + 1 >= WORLD_HEIGHT) {
        return;
    }

    // Tomber (le sable coule dans l'eau).
    const uint8_t below = cell_type(x, y + 1);
    if (below == EMPTY || below == WATER) {
        move_cell(x, y, x, y + 1);
        return;
    }

    // Diagonales, cote aleatoire en premier. On exige aussi que la cellule
    // laterale soit franchissable pour ne pas traverser les coins.
    const int first = rng_bool() ? -1 : 1;
    for (int k = 0; k < 2; k++) {
        const int dir = (k == 0) ? first : -first;
        const int nx = x + dir;
        if (nx < 0 || nx >= WORLD_WIDTH) {
            continue;
        }
        const uint8_t diag = cell_type(nx, y + 1);
        const uint8_t side = cell_type(nx, y);
        if ((diag == EMPTY || diag == WATER) && (side == EMPTY || side == WATER)) {
            move_cell(x, y, nx, y + 1);
            return;
        }
    }
}

void FallingSandGrid::move_water(int x, int y) {
    // 1) Chute verticale.
    if (y + 1 < WORLD_HEIGHT && cell_type(x, y + 1) == EMPTY) {
        move_cell(x, y, x, y + 1);
        return;
    }

    // 2) Diagonales basses.
    if (y + 1 < WORLD_HEIGHT) {
        const int dfirst = rng_bool() ? -1 : 1;
        for (int k = 0; k < 2; k++) {
            const int dir = (k == 0) ? dfirst : -dfirst;
            const int nx = x + dir;
            if (nx < 0 || nx >= WORLD_WIDTH) {
                continue;
            }
            if (cell_type(nx, y) == EMPTY && cell_type(nx, y + 1) == EMPTY) {
                move_cell(x, y, nx, y + 1);
                return;
            }
        }
    }

    // 3) Dispersion horizontale : jusqu'a WATER_DISPERSION cellules d'un
    //    coup (l'eau 1 px/frame parait visqueuse). On s'arrete des qu'un
    //    trou est trouve pour y tomber a la frame suivante.
    //    Note : une goutte totalement entouree ne bouge plus -> le chunk
    //    s'endort ; seule la surface d'une flaque reste active, donc les
    //    dirty rects d'une flaque au repos se reduisent a une fine bande.
    const int hfirst = rng_bool() ? 1 : -1;
    for (int k = 0; k < 2; k++) {
        const int dir = (k == 0) ? hfirst : -hfirst;
        int nx = x;
        int moved = 0;
        while (moved < WATER_DISPERSION) {
            const int tx = nx + dir;
            if (tx < 0 || tx >= WORLD_WIDTH || cell_type(tx, y) != EMPTY) {
                break;
            }
            nx = tx;
            moved++;
            if (y + 1 < WORLD_HEIGHT && cell_type(nx, y + 1) == EMPTY) {
                break; // trou trouve : on s'arrete au-dessus.
            }
        }
        if (moved > 0) {
            move_cell(x, y, nx, y);
            return;
        }
    }
}

// ===========================================================================
//  Deplacement + dirty rects
// ===========================================================================

void FallingSandGrid::move_cell(int x1, int y1, int x2, int y2) {
    const int i = get_index(x1, y1);
    const int j = get_index(x2, y2);

    const uint8_t a = grid[i] & TYPE_MASK;
    const uint8_t b = grid[j] & TYPE_MASK;

    // La particule qui arrive (et celle qui est deplacee, ex. l'eau que le
    // sable ecarte) est marquee "deja simulee" pour cette frame.
    grid[j] = a | UPDATED_BIT;
    grid[i] = (b == EMPTY) ? (uint8_t)EMPTY : (uint8_t)(b | UPDATED_BIT);

    mark_dirty(x1, y1);
    mark_dirty(x2, y2);
}

void FallingSandGrid::mark_dirty(int x, int y) {
    // Dilatation de 1 px : les voisins doivent pouvoir reagir a la frame
    // suivante (un grain qui part reveille celui qui etait pose dessus).
    const int x0 = std::max(x - 1, 0);
    const int y0 = std::max(y - 1, 0);
    const int x1 = std::min(x + 1, WORLD_WIDTH - 1);
    const int y1 = std::min(y + 1, WORLD_HEIGHT - 1);

    const int cx0 = x0 / CHUNK_SIZE, cx1 = x1 / CHUNK_SIZE;
    const int cy0 = y0 / CHUNK_SIZE, cy1 = y1 / CHUNK_SIZE;

    // La zone 3x3 peut chevaucher jusqu'a 4 chunks : chacun recoit la partie
    // du rectangle qui lui appartient (clampee a ses bornes).
    for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
            Chunk &c = chunks[cy * CHUNKS_X + cx];
            atomic_fetch_min(c.w_min_x, std::max(x0, cx * CHUNK_SIZE));
            atomic_fetch_min(c.w_min_y, std::max(y0, cy * CHUNK_SIZE));
            atomic_fetch_max(c.w_max_x, std::min(x1, (cx + 1) * CHUNK_SIZE - 1));
            atomic_fetch_max(c.w_max_y, std::min(y1, (cy + 1) * CHUNK_SIZE - 1));
        }
    }
}

// ===========================================================================
//  Rendu
// ===========================================================================

void FallingSandGrid::render_grid() {
    // Upload brut de la grille (types + bit 7, le shader masque le bit).
    // Un memcpy + un upload de texture : c'est tout.
    std::memcpy(pixel_bytes.ptrw(), grid.data(), grid.size());
    image->set_data(WORLD_WIDTH, WORLD_HEIGHT, false, Image::FORMAT_R8, pixel_bytes);
    texture->update(image);
}

void FallingSandGrid::_draw() {
    // Curseur de pinceau.
    const Vector2 mouse = get_local_mouse_position();
    if (mouse.y < (real_t)(WORLD_HEIGHT * CELL_SIZE)) {
        draw_arc(mouse, (real_t)(brush_radius * CELL_SIZE), 0.0f, 6.2831853f, 48,
                 Color(1.0f, 1.0f, 1.0f, 0.6f), 1.0f);
    }

    if (!debug_overlay) {
        return;
    }

    // Chunks actifs (jaune) + leurs dirty rects (rouge).
    for (const Rect2i &r : debug_chunk_rects) {
        draw_rect(Rect2((real_t)(r.position.x * CELL_SIZE), (real_t)(r.position.y * CELL_SIZE),
                        (real_t)(r.size.x * CELL_SIZE), (real_t)(r.size.y * CELL_SIZE)),
                  Color(1.0f, 1.0f, 0.0f, 0.35f), false, 1.0f);
    }
    for (const Rect2i &r : debug_dirty_rects) {
        draw_rect(Rect2((real_t)(r.position.x * CELL_SIZE), (real_t)(r.position.y * CELL_SIZE),
                        (real_t)(r.size.x * CELL_SIZE), (real_t)(r.size.y * CELL_SIZE)),
                  Color(1.0f, 0.15f, 0.15f, 0.9f), false, 1.0f);
    }
}

// ===========================================================================
//  UI
// ===========================================================================

void FallingSandGrid::setup_ui() {
    ui_panel = memnew(Panel);
    ui_panel->set_position(Vector2(0, (real_t)(WORLD_HEIGHT * CELL_SIZE)));
    ui_panel->set_size(Vector2((real_t)(WORLD_WIDTH * CELL_SIZE), (real_t)UI_HEIGHT));
    add_child(ui_panel);

    const int button_width = 150;
    const int button_height = 60;
    const int button_spacing = 20;
    const int start_x = 50;
    const int start_y = WORLD_HEIGHT * CELL_SIZE + 20;

    for (int i = 0; i < 4; i++) {
        Button *button = memnew(Button);
        button->set_text(PARTICLE_INFOS[i].name);
        button->set_position(Vector2((real_t)(start_x + i * (button_width + button_spacing)), (real_t)start_y));
        button->set_size(Vector2((real_t)button_width, (real_t)button_height));

        button->connect("pressed",
                        Callable(this, "on_particle_button_pressed")
                                .bind((int)PARTICLE_INFOS[i].type));

        add_child(button);
        ui_buttons[PARTICLE_INFOS[i].type] = button;
    }

    stats_label = memnew(Label);
    stats_label->set_position(Vector2((real_t)(start_x + 4 * (button_width + button_spacing)), 12.0f));
    stats_label->set_size(Vector2((real_t)(WORLD_WIDTH * CELL_SIZE - (start_x + 4 * (button_width + button_spacing)) - 10), (real_t)(UI_HEIGHT - 24)));
    ui_panel->add_child(stats_label);

    update_button_styles();
}

void FallingSandGrid::update_button_styles() {
    for (int i = 0; i < 4; i++) {
        Button *button = ui_buttons[PARTICLE_INFOS[i].type];
        const bool selected = (PARTICLE_INFOS[i].type == selected_particle);
        button->add_theme_stylebox_override("normal", make_button_style(PARTICLE_INFOS[i].color, selected, 0.0f));
        button->add_theme_stylebox_override("hover", make_button_style(PARTICLE_INFOS[i].color, selected, 0.15f));
        button->add_theme_stylebox_override("pressed", make_button_style(PARTICLE_INFOS[i].color, selected, -0.15f));
    }
}

void FallingSandGrid::update_stats() {
    if (stats_label == nullptr) {
        return;
    }
    frame_counter++;
    if (frame_counter % 10 != 0) {
        return; // 6x par seconde suffit largement.
    }

    const double fps = Engine::get_singleton()->get_frames_per_second();

    String txt;
    txt += String("FPS: ") + String::num_int64((int64_t)std::lround(fps));
    txt += String("   |   Chunks actifs: ") + String::num_int64(active_chunk_count) + String(" / ") + String::num_int64(NUM_CHUNKS);
    txt += String("   |   Cellules simulees: ") + String::num_int64(dirty_cell_count) + String(" / ") + String::num_int64((int64_t)WORLD_WIDTH * WORLD_HEIGHT);
    txt += String("   |   Threads: ") + String(use_threads ? "ON" : "OFF");
    txt += String("   |   Pinceau: ") + String::num_int64(brush_radius);
    txt += String("\n[Clic gauche] peindre    [Molette] taille pinceau    [D] debug chunks    [T] threads ON/OFF    [C] tout effacer");
    stats_label->set_text(txt);
}

void FallingSandGrid::on_particle_button_pressed(int particle_type) {
    selected_particle = (Particle)particle_type;
    update_button_styles();
}

// ===========================================================================
//  Interaction
// ===========================================================================

void FallingSandGrid::_input(const Ref<InputEvent> &event) {
    Ref<InputEventMouseButton> mb = event;
    if (mb.is_valid()) {
        if (mb->get_button_index() == MOUSE_BUTTON_LEFT) {
            if (mb->is_pressed()) {
                painting = true;
                const Vector2i cell = mouse_to_cell();
                paint_at(cell);
                last_paint_cell = cell;
            } else {
                painting = false;
            }
        } else if (mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_WHEEL_UP) {
            brush_radius = std::min(brush_radius + 1, 40);
        } else if (mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_WHEEL_DOWN) {
            brush_radius = std::max(brush_radius - 1, 1);
        }
        return;
    }

    Ref<InputEventMouseMotion> mm = event;
    if (mm.is_valid()) {
        if (painting && mm->get_button_mask().has_flag(MOUSE_BUTTON_MASK_LEFT)) {
            const Vector2i cell = mouse_to_cell();
            paint_line(last_paint_cell, cell); // interpolation -> pas de pointilles
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
    const Vector2 m = get_local_mouse_position();
    return Vector2i((int)std::floor(m.x / (float)CELL_SIZE),
                    (int)std::floor(m.y / (float)CELL_SIZE));
}

void FallingSandGrid::paint_at(Vector2i cell) {
    const int r = brush_radius;
    const int r2 = r * r;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) {
                continue;
            }
            const int x = cell.x + dx;
            const int y = cell.y + dy;
            if (!in_world(x, y)) {
                continue;
            }
            grid[get_index(x, y)] = (uint8_t)selected_particle;
            mark_dirty(x, y); // reveille la zone (et ses voisins).
        }
    }
}

void FallingSandGrid::paint_line(Vector2i from, Vector2i to) {
    const int steps = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y));
    if (steps == 0) {
        paint_at(to);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        const float t = (float)i / (float)steps;
        const Vector2i p((int)std::lround(from.x + (to.x - from.x) * t),
                         (int)std::lround(from.y + (to.y - from.y) * t));
        paint_at(p);
    }
}

void FallingSandGrid::clear_world() {
    std::fill(grid.begin(), grid.end(), (uint8_t)EMPTY);
    for (int ci = 0; ci < NUM_CHUNKS; ci++) {
        chunks[ci].reset_working();
        chunks[ci].min_x = 0;
        chunks[ci].min_y = 0;
        chunks[ci].max_x = -1;
        chunks[ci].max_y = -1;
    }
}
