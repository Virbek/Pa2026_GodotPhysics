#include "falling_sand_grid.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/display_server.hpp>

using namespace godot;

FallingSandGrid::FallingSandGrid() {
    // Initialize grid with empty cells
    grid.resize(GRID_WIDTH * GRID_HEIGHT, EMPTY);
}

FallingSandGrid::~FallingSandGrid() {
}

void FallingSandGrid::_bind_methods() {
    ClassDB::bind_method(D_METHOD("on_particle_button_pressed", "particle_type"), 
                        &FallingSandGrid::on_particle_button_pressed);
}

void FallingSandGrid::_ready() {
    // Set window size
    DisplayServer::get_singleton()->window_set_size(
        Vector2i(GRID_WIDTH * CELL_SIZE, GRID_HEIGHT * CELL_SIZE + UI_HEIGHT)
    );
    
    // Setup rendering
    image = Image::create(GRID_WIDTH, GRID_HEIGHT, false, Image::FORMAT_RGB8);
    texture = ImageTexture::create_from_image(image);
    
    sprite = memnew(Sprite2D);
    sprite->set_texture(texture);
    sprite->set_centered(false);
    sprite->set_scale(Vector2(CELL_SIZE, CELL_SIZE));
    add_child(sprite);
    
    // Setup UI
    setup_ui();
    
    // Initial render
    render_grid();
}

void FallingSandGrid::_process(double delta) {
    update_grid();
    render_grid();
}

void FallingSandGrid::_input(const Ref<InputEvent>& event) {
    Ref<InputEventMouseButton> mb = event;
    Ref<InputEventMouseMotion> mm = event;
    
    if (mb.is_valid()) {
        if (mb->is_pressed() && mb->get_button_index() == MOUSE_BUTTON_LEFT) {
            place_particle_at_mouse();
        }
    } else if (mm.is_valid()) {
        if (mm->get_button_mask() & MOUSE_BUTTON_MASK_LEFT) {
            place_particle_at_mouse();
        }
    }
}

void FallingSandGrid::setup_ui() {
    ui_panel = memnew(Panel);
    ui_panel->set_position(Vector2(0, GRID_HEIGHT * CELL_SIZE));
    ui_panel->set_size(Vector2(GRID_WIDTH * CELL_SIZE, UI_HEIGHT));
    add_child(ui_panel);
    
    struct ParticleInfo {
        Particle type;
        String name;
        Color color;
    };
    
    ParticleInfo particles[] = {
        {SAND, "Sand", Color(0.96f, 0.64f, 0.38f)},
        {WATER, "Water", Color(0.0f, 0.75f, 1.0f)},
        {ROCK, "Rock", Color(0.41f, 0.41f, 0.41f)},
        {EMPTY, "Erase", Color(0.0f, 0.0f, 0.0f)}
    };
    
    int button_width = 150;
    int button_height = 60;
    int button_spacing = 20;
    int start_x = 50;
    int start_y = GRID_HEIGHT * CELL_SIZE + 20;
    
    for (int i = 0; i < 4; i++) {
        Button* button = memnew(Button);
        button->set_text(particles[i].name);
        button->set_position(Vector2(start_x + i * (button_width + button_spacing), start_y));
        button->set_size(Vector2(button_width, button_height));
        
        Ref<StyleBoxFlat> style = memnew(StyleBoxFlat);
        style->set_bg_color(particles[i].color);
        button->add_theme_stylebox_override("normal", style);
        
        // Passer un int au lieu de Particle
        button->connect("pressed", 
                       Callable(this, "on_particle_button_pressed").bind(static_cast<int>(particles[i].type)));
        
        add_child(button);
        ui_buttons[particles[i].type] = button;
    }
    
    update_button_styles();
}

void FallingSandGrid::on_particle_button_pressed(int particle_type) {
    selected_particle = static_cast<Particle>(particle_type);
    update_button_styles();
}

void FallingSandGrid::update_button_styles() {
    struct ParticleInfo {
        Particle type;
        Color color;
    };
    
    ParticleInfo particles[] = {
        {SAND, Color(0.96f, 0.64f, 0.38f)},
        {WATER, Color(0.0f, 0.75f, 1.0f)},
        {ROCK, Color(0.41f, 0.41f, 0.41f)},
        {EMPTY, Color(0.0f, 0.0f, 0.0f)}
    };
    
    for (int i = 0; i < 4; i++) {
        Button* button = ui_buttons[particles[i].type];
        Ref<StyleBoxFlat> style = memnew(StyleBoxFlat);
        
        if (particles[i].type == selected_particle) {
            style->set_bg_color(particles[i].color.lightened(0.3f));
            style->set_border_width_all(3);
            style->set_border_color(Color(1.0f, 1.0f, 1.0f));
        } else {
            style->set_bg_color(particles[i].color);
        }
        
        button->add_theme_stylebox_override("normal", style);
    }
}

void FallingSandGrid::update_grid() {
    // Process bottom-to-top
    for (int y = GRID_HEIGHT - 1; y >= 0; y--) {
        // Randomize horizontal direction
        bool left_to_right = UtilityFunctions::randf() > 0.5f;
        int start = left_to_right ? 0 : GRID_WIDTH - 1;
        int end = left_to_right ? GRID_WIDTH : -1;
        int step = left_to_right ? 1 : -1;
        
        for (int x = start; x != end; x += step) {
            uint8_t particle = grid[get_index(x, y)];
            
            switch (particle) {
                case SAND:
                    move_sand(x, y);
                    break;
                case WATER:
                    move_water(x, y);
                    break;
                case ROCK:
                    // Rock doesn't move
                    break;
            }
        }
    }
}

void FallingSandGrid::move_sand(int x, int y) {
    // Try to move down
    if (can_move_to(x, y + 1) && is_empty_or_water(x, y + 1)) {
        swap_particles(x, y, x, y + 1);
        return;
    }
    
    // Try diagonal
    bool try_left_first = UtilityFunctions::randf() > 0.5f;
    
    if (try_left_first) {
        if (try_move_diagonal(x, y, -1, true)) return;
        if (try_move_diagonal(x, y, 1, true)) return;
    } else {
        if (try_move_diagonal(x, y, 1, true)) return;
        if (try_move_diagonal(x, y, -1, true)) return;
    }
}

void FallingSandGrid::move_water(int x, int y) {
    // Try to move down
    if (can_move_to(x, y + 1) && grid[get_index(x, y + 1)] == EMPTY) {
        swap_particles(x, y, x, y + 1);
        return;
    }
    
    // Try diagonal down
    bool try_left_first = UtilityFunctions::randf() > 0.5f;
    
    if (try_left_first) {
        if (try_move_diagonal(x, y, -1, false)) return;
        if (try_move_diagonal(x, y, 1, false)) return;
    } else {
        if (try_move_diagonal(x, y, 1, false)) return;
        if (try_move_diagonal(x, y, -1, false)) return;
    }
    
    // Try horizontal spread
    int dir = UtilityFunctions::randf() > 0.5f ? 1 : -1;
    if (can_move_to(x + dir, y) && grid[get_index(x + dir, y)] == EMPTY) {
        swap_particles(x, y, x + dir, y);
        return;
    }
}

bool FallingSandGrid::try_move_diagonal(int x, int y, int dir, bool check_water) {
    int new_x = x + dir;
    int new_y = y + 1;
    
    if (!can_move_to(new_x, new_y)) {
        return false;
    }
    
    uint8_t target = grid[get_index(new_x, new_y)];
    
    if (check_water) {
        // Sand can move into empty or water
        if (target == EMPTY || target == WATER) {
            swap_particles(x, y, new_x, new_y);
            return true;
        }
    } else {
        // Water can only move into empty
        if (target == EMPTY) {
            swap_particles(x, y, new_x, new_y);
            return true;
        }
    }
    
    return false;
}

void FallingSandGrid::render_grid() {
    Color colors[] = {
        Color(0.0f, 0.0f, 0.0f),      // BLACK - EMPTY
        Color(0.96f, 0.64f, 0.38f),   // SANDY_BROWN - SAND
        Color(0.0f, 0.75f, 1.0f),     // DEEP_SKY_BLUE - WATER
        Color(0.41f, 0.41f, 0.41f)    // DIM_GRAY - ROCK
    };
    
    for (int y = 0; y < GRID_HEIGHT; y++) {
        for (int x = 0; x < GRID_WIDTH; x++) {
            uint8_t particle = grid[get_index(x, y)];
            image->set_pixel(x, y, colors[particle]);
        }
    }
    
    texture->update(image);
}

void FallingSandGrid::place_particle_at_mouse() {
    Vector2 mouse_pos = get_local_mouse_position();
    int grid_x = static_cast<int>(mouse_pos.x / CELL_SIZE);
    int grid_y = static_cast<int>(mouse_pos.y / CELL_SIZE);
    
    if (grid_x >= 0 && grid_x < GRID_WIDTH && grid_y >= 0 && grid_y < GRID_HEIGHT) {
        place_particle_area(grid_x, grid_y, 5);
    }
}

void FallingSandGrid::place_particle_area(int center_x, int center_y, int radius) {
    for (int y = center_y - radius; y <= center_y + radius; y++) {
        for (int x = center_x - radius; x <= center_x + radius; x++) {
            if (x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT) {
                float dist = Vector2(x - center_x, y - center_y).length();
                if (dist <= radius) {
                    grid[get_index(x, y)] = static_cast<uint8_t>(selected_particle);
                }
            }
        }
    }
}