#pragma once

#include <vector>
#include <map>

#include <godot_cpp/classes/node2d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/sprite2d.hpp>
#include <godot_cpp/classes/panel.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

enum Particle {
    EMPTY = 0,
    SAND = 1,
    WATER = 2,
    ROCK = 3
};

class FallingSandGrid : public Node2D {
    GDCLASS(FallingSandGrid, Node2D)

private:
    static const int GRID_WIDTH = 400;
    static const int GRID_HEIGHT = 200;
    static const int CELL_SIZE = 2;
    static const int UI_HEIGHT = 100;

    // Grid data - using 1D array for better cache locality
    std::vector<uint8_t> grid;
    
    Particle selected_particle = SAND;
    
    // Godot nodes
    Ref<Image> image;
    Ref<ImageTexture> texture;
    Sprite2D* sprite = nullptr;
    Panel* ui_panel = nullptr;
    std::map<Particle, Button*> ui_buttons;

    // Helper functions
    inline int get_index(int x, int y) const {
        return y * GRID_WIDTH + x;
    }

    inline bool can_move_to(int x, int y) const {
        return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
    }

    inline bool is_empty_or_water(int x, int y) const {
        return grid[get_index(x, y)] == EMPTY || grid[get_index(x, y)] == WATER;
    }

    inline void swap_particles(int x1, int y1, int x2, int y2) {
        std::swap(grid[get_index(x1, y1)], grid[get_index(x2, y2)]);
    }

    void move_sand(int x, int y);
    void move_water(int x, int y);
    bool try_move_diagonal(int x, int y, int dir, bool check_water = false);
    
    void setup_ui();
    void update_button_styles();
    
    
    void place_particle_at_mouse();
    void place_particle_area(int center_x, int center_y, int radius);
    
    void render_grid();
    void update_grid();

protected:
    static void _bind_methods();

public:
    FallingSandGrid();
    ~FallingSandGrid();

    void _ready() override;
    void _process(double delta) override;
    void _input(const Ref<InputEvent>& event) override;

    void on_particle_button_pressed(int particle_type);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::Particle);
