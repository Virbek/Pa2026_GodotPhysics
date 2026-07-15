#pragma once

// ============================================================================
// FallingSandGrid — automate cellulaire 2D inspire de Noita.
//
// Points importants :
//  * grille et cell_data sont deux tableaux 1D contigus ;
//  * le monde est decoupe en chunks avec dirty rects ;
//  * les rangees de chunks sont simulees du bas vers le haut, chacune en
//    damier pair/impair sur X via le WorkerThreadPool ;
//  * UPDATED_BIT interdit de simuler deux fois une particule dans une frame ;
//  * la texture R8 contient directement le type de chaque cellule ;
//  * cell_data stocke la duree de vie du feu/des gaz ou le temps de
//    stabilisation restant d'un liquide.
// ============================================================================

#include <atomic>
#include <climits>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/panel.hpp>
#include <godot_cpp/classes/sprite2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>

namespace godot {

enum Particle : uint8_t {
    EMPTY = 0,
    SAND = 1,
    WATER = 2,
    ROCK = 3,
    SMOKE = 4,
    FIRE = 5,
    WOOD = 6,
    LAVA = 7,
    ACID = 8,
    OIL = 9,
    STEAM = 10
};

class FallingSandGrid : public Node2D {
    GDCLASS(FallingSandGrid, Node2D)

public:
    static const int WORLD_WIDTH = 768;
    static const int WORLD_HEIGHT = 384;
    static const int CELL_SIZE = 2;
    static const int UI_HEIGHT = 100;

    static const int CHUNK_SIZE = 64;
    static const int CHUNKS_X = WORLD_WIDTH / CHUNK_SIZE;
    static const int CHUNKS_Y = WORLD_HEIGHT / CHUNK_SIZE;
    static const int NUM_CHUNKS = CHUNKS_X * CHUNKS_Y;

    // La gravite impose de simuler les cellules du bas vers le haut : une
    // particule ne peut tomber que si celle du dessous a deja bouge. L'ordre
    // des passes respecte donc les rangees de chunks de bas en haut, et chaque
    // rangee est coupee en deux passes (X pair puis X impair) pour que deux
    // chunks simules en parallele restent separes de CHUNK_SIZE cellules.
    static const int NUM_PASSES = CHUNKS_Y * 2;

    // Un liquide ne peut avancer que la ou il voit du vide sur sa propre ligne :
    // au milieu d'une large surface plane il est immobile, et une flaque ne se
    // nivelle qu'en s'erodant par ses bords, ~2 cellules par frame. On rejoue
    // donc les liquides plusieurs fois par frame pour accelerer d'autant. Les
    // solides et les gaz ne sont pas rejoues, sinon toute la simulation serait
    // simplement en avance rapide. Cout CPU proportionnel quand l'eau bouge.
    static const int LIQUID_SUBSTEPS = 3;

    // L'eau compare les deux directions et peut parcourir plusieurs cellules
    // pour trouver un bord ou un trou. Cette valeur est aussi le mouvement
    // horizontal maximal de toute particule pendant une frame.
    static const int WATER_DISPERSION = 30;
    static const int OIL_DISPERSION = 11;
    static const int ACID_DISPERSION = 9;
    static const int LAVA_DISPERSION = 5;
    static const int GAS_DISPERSION = 6;
    static const int MAX_MOVE_DISTANCE = WATER_DISPERSION;

    static const uint8_t TYPE_MASK = 0x7F;
    static const uint8_t UPDATED_BIT = 0x80;

private:
    struct Chunk {
        std::atomic<int> w_min_x{INT_MAX};
        std::atomic<int> w_min_y{INT_MAX};
        std::atomic<int> w_max_x{INT_MIN};
        std::atomic<int> w_max_y{INT_MIN};

        int min_x = 0;
        int min_y = 0;
        int max_x = -1;
        int max_y = -1;

        inline bool active() const { return max_x >= min_x && max_y >= min_y; }

        inline void reset_working() {
            w_min_x.store(INT_MAX, std::memory_order_relaxed);
            w_min_y.store(INT_MAX, std::memory_order_relaxed);
            w_max_x.store(INT_MIN, std::memory_order_relaxed);
            w_max_y.store(INT_MIN, std::memory_order_relaxed);
        }
    };

    struct FlowCandidate {
        int x = 0;
        int distance = 0;
        bool has_drop = false;
    };

    // Donnees de simulation.
    std::vector<uint8_t> grid;
    std::vector<uint8_t> cell_data;
    std::unique_ptr<Chunk[]> chunks;
    std::vector<int> pass_lists[NUM_PASSES];
    int current_pass = 0;
    bool liquids_only = false;

    // Stats / debug.
    int active_chunk_count = 0;
    int64_t dirty_cell_count = 0;
    bool use_threads = true;
    bool debug_overlay = false;
    int frame_counter = 0;
    std::vector<Rect2i> debug_chunk_rects;
    std::vector<Rect2i> debug_dirty_rects;

    // Pinceau.
    Particle selected_particle = SAND;
    int brush_radius = 6;
    bool painting = false;
    Vector2i last_paint_cell;

    // Rendu.
    PackedByteArray pixel_bytes;
    Ref<Image> image;
    Ref<ImageTexture> texture;
    Sprite2D *sprite = nullptr;

    // UI.
    Panel *ui_panel = nullptr;
    Label *stats_label = nullptr;
    std::map<Particle, Button *> ui_buttons;

    static inline int get_index(int x, int y) { return y * WORLD_WIDTH + x; }

    static inline bool in_world(int x, int y) {
        return x >= 0 && x < WORLD_WIDTH && y >= 0 && y < WORLD_HEIGHT;
    }

    inline Particle cell_type(int x, int y) const {
        return static_cast<Particle>(grid[get_index(x, y)] & TYPE_MASK);
    }

    static bool is_solid(Particle type);
    static bool is_liquid(Particle type);
    static bool is_gas(Particle type);
    static bool is_flammable(Particle type);
    static int density(Particle type);
    static int liquid_dispersion(Particle type);

    bool can_sink_into(Particle moving, Particle target) const;
    bool can_gas_rise_into(Particle moving, Particle target) const;

    uint8_t initial_cell_data(Particle type) const;
    void set_cell(int x, int y, Particle type, uint8_t data, bool updated);
    void mark_dirty(int x, int y, int radius_x = 1, int radius_y = 1);
    void move_cell(int x1, int y1, int x2, int y2);
    void keep_liquid_awake(int x, int y);

    // reset_working=false : conserve le rect de travail accumule. Indispensable
    // pour les sous-pas liquides, sinon les particules non rejouees (sable,
    // gaz) perdent leur marquage, leur chunk s'endort et elles se figent.
    void begin_frame(bool reset_working = true);
    void run_simulation();
    void simulate_chunk_cells(const Chunk &chunk);

    void move_sand(int x, int y);
    void move_liquid(int x, int y, Particle type);
    void move_smoke_or_steam(int x, int y, Particle type);
    void move_fire(int x, int y);

    FlowCandidate scan_liquid_side(int x, int y, Particle type, int direction, int max_distance) const;
    bool try_move_gas(int x, int y, Particle type, int horizontal_dispersion);

    bool process_water_reactions(int x, int y);
    bool process_lava_reactions(int x, int y);
    bool process_acid_reactions(int x, int y);
    bool process_fire_reactions(int x, int y);
    void spawn_steam_near(int x, int y);

    void render_grid();
    void setup_material();
    void setup_ui();
    void update_button_styles();
    void update_stats();

    Vector2i mouse_to_cell() const;
    void paint_at(Vector2i cell);
    void paint_line(Vector2i from, Vector2i to);
    void clear_world();

protected:
    static void _bind_methods();

public:
    FallingSandGrid();
    ~FallingSandGrid();

    void _ready() override;
    void _process(double delta) override;
    void _input(const Ref<InputEvent> &event) override;
    void _draw() override;

    void on_particle_button_pressed(int particle_type);
    void _simulate_chunk(int list_index);
};

} // namespace godot
