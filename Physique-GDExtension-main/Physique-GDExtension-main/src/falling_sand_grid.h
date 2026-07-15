#pragma once

// ============================================================================
//  FallingSandGrid — simulation "falling sand" façon Noita (GDC 2019)
//
//  Architecture :
//   * Le monde est découpé en chunks de CHUNK_SIZE x CHUNK_SIZE.
//   * Chaque chunk garde un "dirty rect" : le rectangle englobant des
//     cellules qui ont bougé à la frame précédente (dilaté de 1 pixel).
//     Un chunk sans dirty rect est entièrement ignoré -> coût nul au repos.
//   * La simulation est multithreadée en damier 2x2 (4 passes) : deux chunks
//     simulés en parallèle sont toujours séparés d'un chunk complet, donc
//     aucune cellule ne peut être écrite par deux threads à la fois,
//     sans aucun mutex sur la grille.
//   * Le bit 7 de chaque cellule ("UPDATED_BIT") empêche une particule qui a
//     traversé une frontière de chunk d'être simulée deux fois par frame.
//   * Rendu : la grille brute est uploadée telle quelle dans une texture R8,
//     un shader canvas_item fait le mapping type -> couleur sur GPU
//     (plus aucun set_pixel).
// ============================================================================

#include <atomic>
#include <climits>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/sprite2d.hpp>
#include <godot_cpp/classes/panel.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

enum Particle {
    EMPTY = 0,
    SAND  = 1,
    WATER = 2,
    ROCK  = 3
};

class FallingSandGrid : public Node2D {
    GDCLASS(FallingSandGrid, Node2D)

public:
    // ------------------------------------------------------------------
    // Constantes de tuning.
    // WORLD_WIDTH / WORLD_HEIGHT doivent être des multiples de CHUNK_SIZE.
    // Fenêtre résultante : WORLD * CELL_SIZE (+ UI_HEIGHT en bas).
    // Pour un monde énorme (2048x1024, CELL_SIZE 1), il faudra ajouter
    // une Camera2D — voir README.
    // ------------------------------------------------------------------
    static const int WORLD_WIDTH  = 768;
    static const int WORLD_HEIGHT = 384;
    static const int CELL_SIZE    = 2;
    static const int UI_HEIGHT    = 100;

    static const int CHUNK_SIZE = 64;
    static const int CHUNKS_X   = WORLD_WIDTH / CHUNK_SIZE;
    static const int CHUNKS_Y   = WORLD_HEIGHT / CHUNK_SIZE;
    static const int NUM_CHUNKS = CHUNKS_X * CHUNKS_Y;

    // Nombre max de cellules qu'une goutte d'eau parcourt horizontalement
    // en une frame. Doit rester petit devant CHUNK_SIZE (voir static_assert)
    // pour garantir la sécurité du damier multithread.
    static const int WATER_DISPERSION = 5;

    // Encodage d'une cellule : bits 0..6 = type, bit 7 = "déjà simulée
    // cette frame".
    static const uint8_t TYPE_MASK   = 0x7F;
    static const uint8_t UPDATED_BIT = 0x80;

private:
    // ------------------------------------------------------------------
    // Un chunk = un dirty rect courant (simulé cette frame) + un dirty
    // rect "en construction" (atomique, rempli par les threads pendant la
    // frame, deviendra le rect courant de la frame suivante).
    // Rect vide <=> max < min.
    // ------------------------------------------------------------------
    struct Chunk {
        std::atomic<int> w_min_x{INT_MAX};
        std::atomic<int> w_min_y{INT_MAX};
        std::atomic<int> w_max_x{INT_MIN};
        std::atomic<int> w_max_y{INT_MIN};

        int min_x = 0, min_y = 0, max_x = -1, max_y = -1;

        inline bool active() const { return max_x >= min_x && max_y >= min_y; }

        inline void reset_working() {
            w_min_x.store(INT_MAX, std::memory_order_relaxed);
            w_min_y.store(INT_MAX, std::memory_order_relaxed);
            w_max_x.store(INT_MIN, std::memory_order_relaxed);
            w_max_y.store(INT_MIN, std::memory_order_relaxed);
        }
    };

    // --- données de simulation ---
    std::vector<uint8_t> grid;
    std::unique_ptr<Chunk[]> chunks;
    std::vector<int> pass_lists[4]; // indices de chunks actifs, par passe damier
    int current_pass = 0;

    // --- stats / debug ---
    int active_chunk_count = 0;
    int64_t dirty_cell_count = 0;
    bool use_threads = true;
    bool debug_overlay = false;
    int frame_counter = 0;
    std::vector<Rect2i> debug_chunk_rects;
    std::vector<Rect2i> debug_dirty_rects;

    // --- pinceau ---
    Particle selected_particle = SAND;
    int brush_radius = 6;
    bool painting = false;
    Vector2i last_paint_cell;

    // --- rendu ---
    PackedByteArray pixel_bytes;
    Ref<Image> image;
    Ref<ImageTexture> texture;
    Sprite2D *sprite = nullptr;

    // --- UI ---
    Panel *ui_panel = nullptr;
    Label *stats_label = nullptr;
    std::map<Particle, Button *> ui_buttons;

    // ------------------------------------------------------------------
    // Helpers grille
    // ------------------------------------------------------------------
    static inline int get_index(int x, int y) { return y * WORLD_WIDTH + x; }

    static inline bool in_world(int x, int y) {
        return x >= 0 && x < WORLD_WIDTH && y >= 0 && y < WORLD_HEIGHT;
    }

    inline uint8_t cell_type(int x, int y) const {
        return grid[get_index(x, y)] & TYPE_MASK;
    }

    // Marque une cellule (et ses 8 voisines) comme "active" pour la frame
    // suivante : dilate les dirty rects de tous les chunks touchés.
    // Thread-safe (min/max atomiques).
    void mark_dirty(int x, int y);

    // Déplace/échange deux cellules, pose UPDATED_BIT, marque les deux
    // positions dirty. Appelé uniquement depuis les threads de simulation.
    void move_cell(int x1, int y1, int x2, int y2);

    // ------------------------------------------------------------------
    // Boucle de simulation
    // ------------------------------------------------------------------
    void begin_frame();     // working rects -> rects courants, clear des bits, listes de passes
    void run_simulation();  // 4 passes damier sur WorkerThreadPool
    void simulate_chunk_cells(const Chunk &c);
    void move_sand(int x, int y);
    void move_water(int x, int y);

    // ------------------------------------------------------------------
    // Rendu / UI / interaction
    // ------------------------------------------------------------------
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

    // Exécuté par WorkerThreadPool : simule le chunk n° `list_index` de la
    // passe courante. Ne touche QUE de la mémoire brute (jamais l'API Godot).
    void _simulate_chunk(int list_index);
};

} // namespace godot
