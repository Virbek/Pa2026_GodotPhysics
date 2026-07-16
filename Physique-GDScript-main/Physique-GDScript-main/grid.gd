extends Node2D

enum Particle {
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
	STEAM = 10,
}

const WORLD_WIDTH: int = 768
const WORLD_HEIGHT: int = 384
const CELL_SIZE: int = 2
const UI_HEIGHT: int = 100

const CHUNK_SIZE: int = 64
const CHUNK_SHIFT: int = 6
const CHUNKS_X: int = 12
const CHUNKS_Y: int = 6
const NUM_CHUNKS: int = CHUNKS_X * CHUNKS_Y

const NUM_PASSES: int = CHUNKS_Y * 2

const LIQUID_SUBSTEPS: int = 3

const WATER_DISPERSION: int = 10
const OIL_DISPERSION: int = 8
const ACID_DISPERSION: int = 4
const LAVA_DISPERSION: int = 2
const GAS_DISPERSION: int = 6
const MAX_MOVE_DISTANCE: int = WATER_DISPERSION

const TYPE_MASK: int = 0x7F
const UPDATED_BIT: int = 0x80

const LIQUID_SETTLE_FRAMES: int = 24

const INT32_MAX: int = 2147483647
const INT32_MIN: int = -2147483648

@export var visual_shader: Shader

const FALLING_SAND_SHADER: String = """
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
		color = mix(
			vec3(0.18, 0.13, 0.04),
			vec3(0.72, 0.52, 0.14),
			0.25 + sheen * 0.65
		);
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
"""

const PARTICLE_INFOS: Array = [
	[Particle.SAND, "Sable", Color(0.96, 0.64, 0.38)],
	[Particle.WATER, "Eau", Color(0.0, 0.62, 1.0)],
	[Particle.ROCK, "Roche", Color(0.41, 0.41, 0.44)],
	[Particle.SMOKE, "Fumee", Color(0.30, 0.31, 0.34)],
	[Particle.FIRE, "Feu", Color(1.0, 0.32, 0.03)],
	[Particle.WOOD, "Bois", Color(0.46, 0.22, 0.06)],
	[Particle.LAVA, "Lave", Color(1.0, 0.16, 0.01)],
	[Particle.ACID, "Acide", Color(0.42, 0.95, 0.03)],
	[Particle.OIL, "Huile", Color(0.24, 0.17, 0.04)],
	[Particle.STEAM, "Vapeur", Color(0.72, 0.84, 0.92)],
	[Particle.EMPTY, "Effacer", Color(0.12, 0.12, 0.14)],
]

const ACID_OFFSETS: Array = [
	Vector2i(-1, -1), Vector2i(0, -1), Vector2i(1, -1),
	Vector2i(-1, 0), Vector2i(1, 0),
	Vector2i(-1, 1), Vector2i(0, 1), Vector2i(1, 1),
]

const STEAM_OFFSETS: Array = [
	Vector2i(0, -1), Vector2i(-1, -1), Vector2i(1, -1),
	Vector2i(-1, 0), Vector2i(1, 0),
	Vector2i(0, -2), Vector2i(-2, -1), Vector2i(2, -1),
]

var grid := PackedByteArray()
var cell_data := PackedByteArray()

var chunk_w_min_x := PackedInt32Array()
var chunk_w_min_y := PackedInt32Array()
var chunk_w_max_x := PackedInt32Array()
var chunk_w_max_y := PackedInt32Array()
var chunk_min_x := PackedInt32Array()
var chunk_min_y := PackedInt32Array()
var chunk_max_x := PackedInt32Array()
var chunk_max_y := PackedInt32Array()

var pass_lists: Array = []
var liquids_only := false

var active_chunk_count: int = 0
var dirty_cell_count: int = 0
var debug_overlay := false
var frame_counter: int = 0
var debug_chunk_rects: Array[Rect2i] = []
var debug_dirty_rects: Array[Rect2i] = []

var last_simulation_ms := 0.0
var last_render_ms := 0.0

var selected_particle: int = Particle.SAND
var brush_radius: int = 6
var painting := false
var last_paint_cell := Vector2i.ZERO

var image: Image
var texture: ImageTexture
var sprite: Sprite2D

var ui_panel: Panel
var stats_label: Label
var fps_button: Button
var ui_buttons := {}

var fps_uncapped := false


func _ready() -> void:
	assert(WORLD_WIDTH % CHUNK_SIZE == 0, "WORLD_WIDTH doit etre un multiple de CHUNK_SIZE")
	assert(WORLD_HEIGHT % CHUNK_SIZE == 0, "WORLD_HEIGHT doit etre un multiple de CHUNK_SIZE")
	assert(CHUNKS_X * CHUNK_SIZE == WORLD_WIDTH and CHUNKS_Y * CHUNK_SIZE == WORLD_HEIGHT)
	assert((1 << CHUNK_SHIFT) == CHUNK_SIZE)
	assert(MAX_MOVE_DISTANCE * 2 + 2 < CHUNK_SIZE, "MAX_MOVE_DISTANCE trop grand")

	Engine.physics_ticks_per_second = 60

	Engine.max_fps = 60
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	fps_uncapped = false

	DisplayServer.window_set_size(
			Vector2i(WORLD_WIDTH * CELL_SIZE, WORLD_HEIGHT * CELL_SIZE + UI_HEIGHT))

	var cell_count := WORLD_WIDTH * WORLD_HEIGHT
	grid.resize(cell_count)
	cell_data.resize(cell_count)

	chunk_w_min_x.resize(NUM_CHUNKS)
	chunk_w_min_y.resize(NUM_CHUNKS)
	chunk_w_max_x.resize(NUM_CHUNKS)
	chunk_w_max_y.resize(NUM_CHUNKS)
	chunk_min_x.resize(NUM_CHUNKS)
	chunk_min_y.resize(NUM_CHUNKS)
	chunk_max_x.resize(NUM_CHUNKS)
	chunk_max_y.resize(NUM_CHUNKS)
	for chunk_index in NUM_CHUNKS:
		reset_chunk_working(chunk_index)
		chunk_min_x[chunk_index] = 0
		chunk_min_y[chunk_index] = 0
		chunk_max_x[chunk_index] = -1
		chunk_max_y[chunk_index] = -1

	pass_lists.resize(NUM_PASSES)
	for pass_index in NUM_PASSES:
		pass_lists[pass_index] = []

	image = Image.create_from_data(WORLD_WIDTH, WORLD_HEIGHT, false, Image.FORMAT_R8, grid)
	texture = ImageTexture.create_from_image(image)

	sprite = Sprite2D.new()
	sprite.texture = texture
	sprite.centered = false
	sprite.scale = Vector2(CELL_SIZE, CELL_SIZE)
	sprite.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	sprite.show_behind_parent = true
	add_child(sprite)

	setup_material()
	setup_ui()
	render_grid()


func setup_material() -> void:
	var shader := visual_shader
	if shader == null:
		shader = Shader.new()
		shader.code = FALLING_SAND_SHADER

	var material := ShaderMaterial.new()
	material.shader = shader
	sprite.material = material


static func is_solid(type: int) -> bool:
	return type == Particle.ROCK or type == Particle.WOOD


static func is_liquid(type: int) -> bool:
	return type == Particle.WATER or type == Particle.LAVA \
			or type == Particle.ACID or type == Particle.OIL


static func is_gas(type: int) -> bool:
	return type == Particle.SMOKE or type == Particle.FIRE or type == Particle.STEAM


static func is_flammable(type: int) -> bool:
	return type == Particle.WOOD or type == Particle.OIL


static func density(type: int) -> int:
	match type:
		Particle.EMPTY: return -1000
		Particle.FIRE: return -40
		Particle.SMOKE: return -30
		Particle.STEAM: return -20
		Particle.OIL: return 10
		Particle.WATER: return 20
		Particle.ACID: return 26
		Particle.LAVA: return 32
		Particle.SAND: return 50
		Particle.ROCK, Particle.WOOD: return 10000
		_: return 0


static func liquid_dispersion(type: int) -> int:
	match type:
		Particle.WATER: return WATER_DISPERSION
		Particle.OIL: return OIL_DISPERSION
		Particle.ACID: return ACID_DISPERSION
		Particle.LAVA: return LAVA_DISPERSION
		_: return 1


func can_sink_into(moving: int, target: int) -> bool:
	if target == Particle.EMPTY:
		return true
	if moving == target or is_solid(target) or target == Particle.SAND:
		return false
	return density(target) < density(moving)


func can_gas_rise_into(moving: int, target: int) -> bool:
	if target == Particle.EMPTY:
		return true
	return is_gas(target) and target != moving and density(target) > density(moving)


func initial_cell_data(type: int) -> int:
	match type:
		Particle.FIRE: return _rng_u8(48, 92)
		Particle.SMOKE: return _rng_u8(100, 190)
		Particle.STEAM: return _rng_u8(75, 145)
		Particle.WATER, Particle.LAVA, Particle.ACID, Particle.OIL:
			return LIQUID_SETTLE_FRAMES
		_: return 0


func _rng_bool() -> bool:
	return (randi() & 1) != 0


func _rng_chance(numerator: int, denominator: int) -> bool:
	return denominator != 0 and (randi() % denominator) < numerator


func _rng_u8(minimum: int, maximum: int) -> int:
	return minimum + (randi() % (maximum - minimum + 1))


# Met à jour la simulation à fréquence fixe et ajoute des sous-pas pour les liquides
func _physics_process(_delta: float) -> void:
	var start_usec := Time.get_ticks_usec()

	begin_frame()
	run_simulation()

	for substep in range(1, LIQUID_SUBSTEPS):
		begin_frame(false)
		liquids_only = true
		run_simulation()
		liquids_only = false

	last_simulation_ms = float(Time.get_ticks_usec() - start_usec) / 1000.0


func _process(_delta: float) -> void:
	var start_usec := Time.get_ticks_usec()
	render_grid()
	last_render_ms = float(Time.get_ticks_usec() - start_usec) / 1000.0

	update_stats()
	queue_redraw()


func reset_chunk_working(chunk_index: int) -> void:
	chunk_w_min_x[chunk_index] = INT32_MAX
	chunk_w_min_y[chunk_index] = INT32_MAX
	chunk_w_max_x[chunk_index] = INT32_MIN
	chunk_w_max_y[chunk_index] = INT32_MIN


# Prépare les chunks actifs et réinitialise leur état pour la nouvelle frame
func begin_frame(reset_working: bool = true) -> void:
	active_chunk_count = 0
	dirty_cell_count = 0
	for pass_index in NUM_PASSES:
		pass_lists[pass_index].clear()
	debug_chunk_rects.clear()
	debug_dirty_rects.clear()

	for chunk_index in NUM_CHUNKS:
		var cmin_x := chunk_w_min_x[chunk_index]
		var cmin_y := chunk_w_min_y[chunk_index]
		var cmax_x := chunk_w_max_x[chunk_index]
		var cmax_y := chunk_w_max_y[chunk_index]
		chunk_min_x[chunk_index] = cmin_x
		chunk_min_y[chunk_index] = cmin_y
		chunk_max_x[chunk_index] = cmax_x
		chunk_max_y[chunk_index] = cmax_y
		if reset_working:
			reset_chunk_working(chunk_index)

		if cmax_x < cmin_x or cmax_y < cmin_y:
			continue

		for y in range(cmin_y, cmax_y + 1):
			var row_base := y * WORLD_WIDTH
			for x in range(cmin_x, cmax_x + 1):
				var index := row_base + x
				grid[index] = grid[index] & TYPE_MASK

		var chunk_x := chunk_index % CHUNKS_X
		@warning_ignore("integer_division")
		var chunk_y := chunk_index / CHUNKS_X
		pass_lists[(CHUNKS_Y - 1 - chunk_y) * 2 + (chunk_x & 1)].append(chunk_index)

		active_chunk_count += 1
		dirty_cell_count += (cmax_x - cmin_x + 1) * (cmax_y - cmin_y + 1)

		if debug_overlay:
			debug_chunk_rects.append(Rect2i(
					chunk_x * CHUNK_SIZE, chunk_y * CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE))
			debug_dirty_rects.append(Rect2i(
					cmin_x, cmin_y, cmax_x - cmin_x + 1, cmax_y - cmin_y + 1))


func run_simulation() -> void:
	for pass_index in NUM_PASSES:
		var list: Array = pass_lists[pass_index]
		for chunk_index in list:
			simulate_chunk_cells(chunk_index)


# Parcourt les cellules de bas en haut pour respecter la gravité
func simulate_chunk_cells(chunk_index: int) -> void:
	var cmin_x := chunk_min_x[chunk_index]
	var cmin_y := chunk_min_y[chunk_index]
	var cmax_x := chunk_max_x[chunk_index]
	var cmax_y := chunk_max_y[chunk_index]

	var y := cmax_y
	while y >= cmin_y:
		var left_to_right := (randi() & 1) != 0
		var x := cmin_x if left_to_right else cmax_x
		var x_end := cmax_x + 1 if left_to_right else cmin_x - 1
		var step := 1 if left_to_right else -1
		var row_base := y * WORLD_WIDTH

		while x != x_end:
			var encoded := grid[row_base + x]
			if encoded != 0 and (encoded & UPDATED_BIT) == 0:
				var type := encoded & TYPE_MASK
				if not liquids_only or is_liquid(type):
					match type:
						Particle.SAND:
							move_sand(x, y)
						Particle.WATER, Particle.LAVA, Particle.ACID, Particle.OIL:
							move_liquid(x, y, type)
						Particle.SMOKE, Particle.STEAM:
							move_smoke_or_steam(x, y, type)
						Particle.FIRE:
							move_fire(x, y)
			x += step
		y -= 1


static func in_world(x: int, y: int) -> bool:
	return x >= 0 and x < WORLD_WIDTH and y >= 0 and y < WORLD_HEIGHT


func cell_type(x: int, y: int) -> int:
	return grid[y * WORLD_WIDTH + x] & TYPE_MASK


func set_cell(x: int, y: int, type: int, data: int, updated: bool) -> void:
	if not in_world(x, y):
		return

	var index := y * WORLD_WIDTH + x
	var value := type
	if updated and type != Particle.EMPTY:
		value |= UPDATED_BIT
	grid[index] = value
	cell_data[index] = 0 if type == Particle.EMPTY else data
	mark_dirty(x, y)


# Déplace ou échange deux particules puis réveille les zones concernées
func move_cell(x1: int, y1: int, x2: int, y2: int) -> void:
	var source_index := y1 * WORLD_WIDTH + x1
	var target_index := y2 * WORLD_WIDTH + x2

	var source_type := grid[source_index] & TYPE_MASK
	var target_type := grid[target_index] & TYPE_MASK
	var source_data := cell_data[source_index]
	var target_data := cell_data[target_index]

	grid[target_index] = source_type | UPDATED_BIT
	cell_data[target_index] = source_data

	if target_type == Particle.EMPTY:
		grid[source_index] = Particle.EMPTY
		cell_data[source_index] = 0
	else:
		grid[source_index] = target_type | UPDATED_BIT
		cell_data[source_index] = target_data

	if is_liquid(source_type):
		cell_data[target_index] = LIQUID_SETTLE_FRAMES
	if is_liquid(target_type):
		cell_data[source_index] = LIQUID_SETTLE_FRAMES

	mark_dirty(x1, y1)
	mark_dirty(x2, y2)


func keep_liquid_awake(x: int, y: int) -> void:
	var index := y * WORLD_WIDTH + x
	if cell_data[index] > 0:
		cell_data[index] = cell_data[index] - 1
		mark_dirty(x, y)


func mark_dirty(x: int, y: int, radius_x: int = 1, radius_y: int = 1) -> void:
	var x0 := maxi(x - radius_x, 0)
	var x1 := mini(x + radius_x, WORLD_WIDTH - 1)
	var y0 := maxi(y - radius_y, 0)
	var y1 := mini(y + radius_y, WORLD_HEIGHT - 1)

	var chunk_x0 := x0 >> CHUNK_SHIFT
	var chunk_x1 := x1 >> CHUNK_SHIFT
	var chunk_y0 := y0 >> CHUNK_SHIFT
	var chunk_y1 := y1 >> CHUNK_SHIFT

	for chunk_y in range(chunk_y0, chunk_y1 + 1):
		for chunk_x in range(chunk_x0, chunk_x1 + 1):
			var chunk_index := chunk_y * CHUNKS_X + chunk_x
			var cell_x0 := maxi(x0, chunk_x << CHUNK_SHIFT)
			var cell_y0 := maxi(y0, chunk_y << CHUNK_SHIFT)
			var cell_x1 := mini(x1, ((chunk_x + 1) << CHUNK_SHIFT) - 1)
			var cell_y1 := mini(y1, ((chunk_y + 1) << CHUNK_SHIFT) - 1)

			if cell_x0 < chunk_w_min_x[chunk_index]:
				chunk_w_min_x[chunk_index] = cell_x0
			if cell_y0 < chunk_w_min_y[chunk_index]:
				chunk_w_min_y[chunk_index] = cell_y0
			if cell_x1 > chunk_w_max_x[chunk_index]:
				chunk_w_max_x[chunk_index] = cell_x1
			if cell_y1 > chunk_w_max_y[chunk_index]:
				chunk_w_max_y[chunk_index] = cell_y1


func move_sand(x: int, y: int) -> void:
	if y + 1 >= WORLD_HEIGHT:
		return

	if can_sink_into(Particle.SAND, cell_type(x, y + 1)):
		move_cell(x, y, x, y + 1)
		return

	var first_direction := -1 if _rng_bool() else 1
	for attempt in 2:
		var direction := first_direction if attempt == 0 else -first_direction
		var next_x := x + direction
		if not in_world(next_x, y + 1):
			continue

		var diagonal := cell_type(next_x, y + 1)
		var side := cell_type(next_x, y)
		if can_sink_into(Particle.SAND, diagonal) and not is_solid(side) and side != Particle.SAND:
			move_cell(x, y, next_x, y + 1)
			return


# Recherche une position horizontale disponible pour étaler un liquide
func scan_liquid_side(x: int, y: int, type: int, direction: int, max_distance: int) -> Vector3i:
	var candidate_x := x
	var candidate_distance := 0
	var candidate_drop := 0

	var crossed_same_liquid := false

	var distance := 1
	while distance <= max_distance:
		var target_x := x + direction * distance

		if target_x < 0 or target_x >= WORLD_WIDTH:
			break

		var target := cell_type(target_x, y)

		if target == type:
			crossed_same_liquid = true
			distance += 1
			continue

		if target == Particle.EMPTY:
			candidate_x = target_x
			candidate_distance = distance
			candidate_drop = 0

			if y + 1 < WORLD_HEIGHT and can_sink_into(type, cell_type(target_x, y + 1)):
				candidate_drop = 1

			if candidate_drop == 1:
				break

			if crossed_same_liquid:
				if _rng_bool():
					break

				candidate_x = x
				candidate_distance = 0
				candidate_drop = 0
				break

			distance += 1
			continue

		if distance == 1 and can_sink_into(type, target):
			candidate_x = target_x
			candidate_distance = 1

		break

	return Vector3i(candidate_x, candidate_distance, candidate_drop)


# Un liquide tombe, glisse en diagonale puis s'étale horizontalement
func move_liquid(x: int, y: int, type: int) -> void:
	if not liquids_only:
		if type == Particle.WATER and process_water_reactions(x, y):
			return
		if type == Particle.LAVA and process_lava_reactions(x, y):
			return
		if type == Particle.ACID and process_acid_reactions(x, y):
			return

	if y + 1 < WORLD_HEIGHT and can_sink_into(type, cell_type(x, y + 1)):
		move_cell(x, y, x, y + 1)
		return

	if y + 1 < WORLD_HEIGHT:
		var first_direction := -1 if _rng_bool() else 1
		for attempt in 2:
			var direction := first_direction if attempt == 0 else -first_direction
			var next_x := x + direction
			if not in_world(next_x, y + 1):
				continue

			var side := cell_type(next_x, y)
			var diagonal := cell_type(next_x, y + 1)
			if can_sink_into(type, diagonal) and not is_solid(side) and side != Particle.SAND:
				move_cell(x, y, next_x, y + 1)
				return

	var dispersion := liquid_dispersion(type)
	var left := scan_liquid_side(x, y, type, -1, dispersion)
	var right := scan_liquid_side(x, y, type, 1, dispersion)

	var chosen := Vector3i(x, 0, 0)
	var has_chosen := false
	if left.z != right.z:
		chosen = left if left.z == 1 else right
		has_chosen = true
	elif left.z == 1 and right.z == 1:
		if left.y == right.y:
			chosen = left if _rng_bool() else right
		else:
			chosen = left if left.y < right.y else right
		has_chosen = true
	elif left.y != right.y:
		chosen = left if left.y > right.y else right
		has_chosen = true
	elif left.y > 0:
		chosen = left if _rng_bool() else right
		has_chosen = true

	if has_chosen and chosen.y > 0:
		move_cell(x, y, chosen.x, y)
		return

	keep_liquid_awake(x, y)


# Les gaz montent en priorité puis se dispersent sur les côtés
func try_move_gas(x: int, y: int, type: int, horizontal_dispersion: int) -> bool:
	if y > 0 and can_gas_rise_into(type, cell_type(x, y - 1)):
		move_cell(x, y, x, y - 1)
		return true

	if y > 0:
		var first_direction := -1 if _rng_bool() else 1
		for attempt in 2:
			var direction := first_direction if attempt == 0 else -first_direction
			var next_x := x + direction
			if not in_world(next_x, y - 1):
				continue
			if can_gas_rise_into(type, cell_type(next_x, y - 1)) \
					and not is_solid(cell_type(next_x, y)):
				move_cell(x, y, next_x, y - 1)
				return true

	var available_left := 0
	var available_right := 0
	for distance in range(1, horizontal_dispersion + 1):
		var target_x := x - distance
		if target_x < 0 or not can_gas_rise_into(type, cell_type(target_x, y)):
			break
		available_left = distance
	for distance in range(1, horizontal_dispersion + 1):
		var target_x := x + distance
		if target_x >= WORLD_WIDTH or not can_gas_rise_into(type, cell_type(target_x, y)):
			break
		available_right = distance

	if available_left == 0 and available_right == 0:
		return false

	var target_x := x
	if available_left == available_right:
		target_x += available_right if _rng_bool() else -available_left
	elif available_left > available_right:
		target_x -= available_left
	else:
		target_x += available_right

	move_cell(x, y, target_x, y)
	return true


func move_smoke_or_steam(x: int, y: int, type: int) -> void:
	var index := y * WORLD_WIDTH + x
	var life := cell_data[index]
	if life == 0:
		life = initial_cell_data(type)

	if life <= 1:
		if type == Particle.STEAM and _rng_chance(1, 3):
			set_cell(x, y, Particle.WATER, LIQUID_SETTLE_FRAMES, true)
		else:
			set_cell(x, y, Particle.EMPTY, 0, false)
		return

	cell_data[index] = life - 1
	if not try_move_gas(x, y, type, GAS_DISPERSION):
		mark_dirty(x, y)


func move_fire(x: int, y: int) -> void:
	if process_fire_reactions(x, y):
		return

	var index := y * WORLD_WIDTH + x
	var life := cell_data[index]
	if life == 0:
		life = initial_cell_data(Particle.FIRE)

	if life <= 1:
		set_cell(x, y, Particle.SMOKE, initial_cell_data(Particle.SMOKE), true)
		return

	cell_data[index] = life - 1
	if not try_move_gas(x, y, Particle.FIRE, 2):
		mark_dirty(x, y)


func process_water_reactions(x: int, y: int) -> bool:
	var extinguished_fire := false

	for offset_y in range(-1, 2):
		for offset_x in range(-1, 2):
			if offset_x == 0 and offset_y == 0:
				continue
			var neighbor_x := x + offset_x
			var neighbor_y := y + offset_y
			if not in_world(neighbor_x, neighbor_y):
				continue
			if cell_type(neighbor_x, neighbor_y) == Particle.FIRE:
				set_cell(neighbor_x, neighbor_y, Particle.STEAM,
						initial_cell_data(Particle.STEAM), true)
				extinguished_fire = true

	if extinguished_fire:
		cell_data[y * WORLD_WIDTH + x] = LIQUID_SETTLE_FRAMES
		mark_dirty(x, y)
	return extinguished_fire


# Gère l'extinction du feu et la propagation aux matériaux inflammables
func process_fire_reactions(x: int, y: int) -> bool:
	for offset_y in range(-1, 2):
		for offset_x in range(-1, 2):
			if offset_x == 0 and offset_y == 0:
				continue
			var neighbor_x := x + offset_x
			var neighbor_y := y + offset_y
			if in_world(neighbor_x, neighbor_y) \
					and cell_type(neighbor_x, neighbor_y) == Particle.WATER:
				set_cell(x, y, Particle.STEAM, initial_cell_data(Particle.STEAM), true)
				return true

	for offset_y in range(-1, 2):
		for offset_x in range(-1, 2):
			if offset_x == 0 and offset_y == 0:
				continue
			var neighbor_x := x + offset_x
			var neighbor_y := y + offset_y
			if not in_world(neighbor_x, neighbor_y):
				continue

			var neighbor := cell_type(neighbor_x, neighbor_y)
			var ignite_wood := neighbor == Particle.WOOD and _rng_chance(1, 42)
			var ignite_oil := neighbor == Particle.OIL and _rng_chance(1, 7)
			if ignite_wood or ignite_oil:
				set_cell(neighbor_x, neighbor_y, Particle.FIRE,
						initial_cell_data(Particle.FIRE), true)

	return false


func process_lava_reactions(x: int, y: int) -> bool:
	var reacted := false

	for offset_y in range(-1, 2):
		for offset_x in range(-1, 2):
			if offset_x == 0 and offset_y == 0:
				continue
			var neighbor_x := x + offset_x
			var neighbor_y := y + offset_y
			if not in_world(neighbor_x, neighbor_y):
				continue

			var neighbor := cell_type(neighbor_x, neighbor_y)
			if neighbor == Particle.WATER:
				set_cell(neighbor_x, neighbor_y, Particle.ROCK, 0, true)
				spawn_steam_near(neighbor_x, neighbor_y)
				reacted = true
			elif is_flammable(neighbor) and _rng_chance(1, 10):
				set_cell(neighbor_x, neighbor_y, Particle.FIRE,
						initial_cell_data(Particle.FIRE), true)
				reacted = true

	if reacted:
		cell_data[y * WORLD_WIDTH + x] = LIQUID_SETTLE_FRAMES
		mark_dirty(x, y)
	return reacted


func process_acid_reactions(x: int, y: int) -> bool:
	var start := randi() % 8

	for step in 8:
		var offset: Vector2i = ACID_OFFSETS[(start + step) % 8]
		var neighbor_x := x + offset.x
		var neighbor_y := y + offset.y
		if not in_world(neighbor_x, neighbor_y):
			continue

		var neighbor := cell_type(neighbor_x, neighbor_y)
		var dissolve := false
		if neighbor == Particle.WOOD:
			dissolve = _rng_chance(1, 10)
		elif neighbor == Particle.SAND:
			dissolve = _rng_chance(1, 18)
		elif neighbor == Particle.ROCK:
			dissolve = _rng_chance(1, 55)

		if dissolve:
			set_cell(neighbor_x, neighbor_y, Particle.EMPTY, 0, false)
			if _rng_chance(1, 12):
				set_cell(x, y, Particle.EMPTY, 0, false)
			else:
				cell_data[y * WORLD_WIDTH + x] = LIQUID_SETTLE_FRAMES
				mark_dirty(x, y)
			return true

	return false


func spawn_steam_near(x: int, y: int) -> void:
	for offset: Vector2i in STEAM_OFFSETS:
		var target_x := x + offset.x
		var target_y := y + offset.y
		if in_world(target_x, target_y) and cell_type(target_x, target_y) == Particle.EMPTY:
			set_cell(target_x, target_y, Particle.STEAM,
					initial_cell_data(Particle.STEAM), true)
			return


# Copie la grille de simulation dans la texture affichée.
func render_grid() -> void:
	image.set_data(WORLD_WIDTH, WORLD_HEIGHT, false, Image.FORMAT_R8, grid)
	texture.update(image)


func _draw() -> void:
	var mouse := get_local_mouse_position()
	if mouse.y < float(WORLD_HEIGHT * CELL_SIZE):
		draw_arc(mouse, float(brush_radius * CELL_SIZE),
				0.0, TAU, 48, Color(1.0, 1.0, 1.0, 0.62), 1.0)

	if not debug_overlay:
		return

	for rect in debug_chunk_rects:
		draw_rect(Rect2(
				rect.position.x * CELL_SIZE, rect.position.y * CELL_SIZE,
				rect.size.x * CELL_SIZE, rect.size.y * CELL_SIZE),
				Color(1.0, 1.0, 0.0, 0.35), false, 1.0)
	for rect in debug_dirty_rects:
		draw_rect(Rect2(
				rect.position.x * CELL_SIZE, rect.position.y * CELL_SIZE,
				rect.size.x * CELL_SIZE, rect.size.y * CELL_SIZE),
				Color(1.0, 0.15, 0.15, 0.90), false, 1.0)


func make_button_style(base: Color, selected: bool, lighten: float) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = base.lightened(0.28 + lighten) if selected else base.lightened(lighten)
	if selected:
		style.set_border_width_all(3)
		style.border_color = Color.WHITE
	return style


func particle_name(type: int) -> String:
	for info in PARTICLE_INFOS:
		if info[0] == type:
			return info[1]
	return "Inconnu"


func setup_ui() -> void:
	ui_panel = Panel.new()
	ui_panel.position = Vector2(0, WORLD_HEIGHT * CELL_SIZE)
	ui_panel.size = Vector2(WORLD_WIDTH * CELL_SIZE, UI_HEIGHT)
	add_child(ui_panel)

	var columns := 6
	var button_width := 112
	var button_height := 34
	var horizontal_gap := 8
	var vertical_gap := 7
	var start_x := 12
	var start_y := 10

	for index in PARTICLE_INFOS.size():
		var column := index % columns
		@warning_ignore("integer_division")
		var row := index / columns

		var button := Button.new()
		button.text = PARTICLE_INFOS[index][1]
		button.position = Vector2(
				start_x + column * (button_width + horizontal_gap),
				start_y + row * (button_height + vertical_gap))
		button.size = Vector2(button_width, button_height)
		button.mouse_filter = Control.MOUSE_FILTER_STOP
		button.pressed.connect(on_particle_button_pressed.bind(int(PARTICLE_INFOS[index][0])))

		ui_panel.add_child(button)
		ui_buttons[PARTICLE_INFOS[index][0]] = button

	fps_button = Button.new()
	fps_button.position = Vector2(
			start_x + 5 * (button_width + horizontal_gap),
			start_y + button_height + vertical_gap)
	fps_button.size = Vector2(button_width, button_height)
	fps_button.mouse_filter = Control.MOUSE_FILTER_STOP
	fps_button.pressed.connect(on_fps_button_pressed)
	ui_panel.add_child(fps_button)
	update_fps_button()

	stats_label = Label.new()
	stats_label.position = Vector2(748, 8)
	stats_label.size = Vector2(WORLD_WIDTH * CELL_SIZE - 760, UI_HEIGHT - 12)
	ui_panel.add_child(stats_label)

	update_button_styles()


func update_button_styles() -> void:
	for info in PARTICLE_INFOS:
		var button: Button = ui_buttons[info[0]]
		var selected: bool = info[0] == selected_particle
		button.add_theme_stylebox_override("normal", make_button_style(info[2], selected, 0.0))
		button.add_theme_stylebox_override("hover", make_button_style(info[2], selected, 0.13))
		button.add_theme_stylebox_override("pressed", make_button_style(info[2], selected, -0.12))


func update_stats() -> void:
	if stats_label == null:
		return

	frame_counter += 1
	if frame_counter % 10 != 0:
		return

	var fps := Engine.get_frames_per_second()

	var text := "FPS: %d" % int(roundf(fps))
	text += "  |  Chunks: %d/%d" % [active_chunk_count, NUM_CHUNKS]
	text += "  |  Cellules: %d" % dirty_cell_count
	text += "  |  Threads: OFF (GDScript)"
	text += "  |  Simulation: 60 TPS"
	text += "  |  Rendu: %s" % ("UNCAP" if fps_uncapped else "60 FPS")
	text += "\nSim: %.2f ms  |  Rendu tex: %.2f ms" % [last_simulation_ms, last_render_ms]
	text += "\nMateriau: %s" % particle_name(selected_particle)
	text += "  |  Pinceau: %d" % brush_radius
	text += "  |  [D] chunks  [C] effacer  [Molette] taille"
	stats_label.text = text


func on_particle_button_pressed(particle_type: int) -> void:
	selected_particle = particle_type
	update_button_styles()


func on_fps_button_pressed() -> void:
	fps_uncapped = not fps_uncapped

	Engine.max_fps = 0 if fps_uncapped else 60
	update_fps_button()


func update_fps_button() -> void:
	if fps_button == null:
		return
	fps_button.text = "FPS : UNCAP" if fps_uncapped else "FPS : 60"


# Gère le dessin des particules, la taille du pinceau et les raccourcis.
func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			if event.pressed:
				painting = true
				var cell := mouse_to_cell()
				paint_at(cell)
				last_paint_cell = cell
			else:
				painting = false
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
			brush_radius = mini(brush_radius + 1, 40)
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			brush_radius = maxi(brush_radius - 1, 1)
		return

	if event is InputEventMouseMotion:
		if painting and (event.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0:
			var cell := mouse_to_cell()
			paint_line(last_paint_cell, cell)
			last_paint_cell = cell
		return

	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_D:
				debug_overlay = not debug_overlay
			KEY_C:
				clear_world()


func mouse_to_cell() -> Vector2i:
	var mouse := get_local_mouse_position()
	return Vector2i(
			floori(mouse.x / float(CELL_SIZE)),
			floori(mouse.y / float(CELL_SIZE)))


func paint_at(cell: Vector2i) -> void:
	var radius_squared := brush_radius * brush_radius

	for offset_y in range(-brush_radius, brush_radius + 1):
		for offset_x in range(-brush_radius, brush_radius + 1):
			if offset_x * offset_x + offset_y * offset_y > radius_squared:
				continue

			var x := cell.x + offset_x
			var y := cell.y + offset_y
			if not in_world(x, y):
				continue

			var index := y * WORLD_WIDTH + x
			grid[index] = selected_particle
			cell_data[index] = initial_cell_data(selected_particle)
			mark_dirty(x, y)


func paint_line(from: Vector2i, to: Vector2i) -> void:
	var steps := maxi(absi(to.x - from.x), absi(to.y - from.y))
	if steps == 0:
		paint_at(to)
		return

	for step in range(steps + 1):
		var ratio := float(step) / float(steps)
		var point := Vector2i(
				roundi(from.x + (to.x - from.x) * ratio),
				roundi(from.y + (to.y - from.y) * ratio))
		paint_at(point)


func clear_world() -> void:
	grid.fill(0)
	cell_data.fill(0)

	for chunk_index in NUM_CHUNKS:
		reset_chunk_working(chunk_index)
		chunk_min_x[chunk_index] = 0
		chunk_min_y[chunk_index] = 0
		chunk_max_x[chunk_index] = -1
		chunk_max_y[chunk_index] = -1


func get_last_simulation_ms() -> float:
	return last_simulation_ms


func get_last_render_ms() -> float:
	return last_render_ms


func get_active_particle_count() -> int:
	return dirty_cell_count
