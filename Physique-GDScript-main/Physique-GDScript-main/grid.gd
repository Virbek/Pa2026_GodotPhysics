extends Node2D

# Dimensions de la grille
const GRID_WIDTH: int = 400
const GRID_HEIGHT: int = 200
const CELL_SIZE: int = 2  # Echelle
const UI_HEIGHT: int = 100 # Partie en bas avec les boutons

# Types de particules
enum Particle {
	EMPTY = 0,
	SAND = 1,
	WATER = 2,
	ROCK = 3,
}

# Particule sélectionnée pour le placement
var selected_particle: Particle = Particle.SAND

# La grille
var grid = []

# Pour le rendu
var image: Image
var texture: ImageTexture
var sprite: Sprite2D

# Pour l'UI
var ui_panel: Panel
var ui_buttons = {}

func _ready():
	# On initialise la grille à zéro
	grid.resize(GRID_HEIGHT)
	for y in range(GRID_HEIGHT):
		grid[y] = []
		grid[y].resize(GRID_WIDTH)
		for x in range(GRID_WIDTH):
			grid[y][x] = Particle.EMPTY
	
	# Mise en place du rendu
	image = Image.create(GRID_WIDTH, GRID_HEIGHT, false, Image.FORMAT_RGB8)
	texture = ImageTexture.create_from_image(image)
	
	sprite = Sprite2D.new()
	sprite.texture = texture
	sprite.centered = false
	sprite.scale = Vector2(CELL_SIZE, CELL_SIZE)
	add_child(sprite)
	
	# Initialisation de l'ui
	setup_ui()
	
	# Premier rendu
	render_grid()
	
func setup_ui():
	# On créé un panel pour le fond de l'ui
	ui_panel = Panel.new()
	ui_panel.position = Vector2(0, GRID_HEIGHT * CELL_SIZE)
	ui_panel.size = Vector2(GRID_WIDTH * CELL_SIZE, UI_HEIGHT)
	add_child(ui_panel)
	
	# Un bouton pour chaque type de particule
	var button_width = 150
	var button_height = 60
	var button_spacing = 20
	var start_x = 50
	var start_y = GRID_HEIGHT * CELL_SIZE + 20
	
	var particles = [
		{"type": Particle.SAND, "name": "Sand", "color": Color.SANDY_BROWN},
		{"type": Particle.WATER, "name": "Water", "color": Color.DEEP_SKY_BLUE},
		{"type": Particle.ROCK, "name": "Rock", "color": Color.DIM_GRAY},
		{"type": Particle.EMPTY, "name": "Erase", "color": Color.BLACK}
	]
	
	for i in range(particles.size()):
		var particle_data = particles[i]
		var button = Button.new()
		button.text = particle_data.name
		button.position = Vector2(start_x + i * (button_width + button_spacing), start_y)
		button.size = Vector2(button_width, button_height)
		
		# Bouton de la même couleur que la particule
		var style = StyleBoxFlat.new()
		style.bg_color = particle_data.color
		button.add_theme_stylebox_override("normal", style)
		
		# La particule sélectionnée est plus claire
		if particle_data.type == selected_particle:
			var selected_style = StyleBoxFlat.new()
			selected_style.bg_color = particle_data.color.lightened(0.3)
			selected_style.set_border_width_all(3) 
			selected_style.border_color = Color.WHITE
			button.add_theme_stylebox_override("normal", selected_style)
		
		# Connection au signal pour changer la particule choisie
		button.pressed.connect(_on_particle_button_pressed.bind(particle_data.type))
		
		add_child(button)
		ui_buttons[particle_data.type] = button
		
func _on_particle_button_pressed(particle_type: Particle):
	selected_particle = particle_type
	
	var particles = [
		{"type": Particle.SAND, "color": Color.SANDY_BROWN},
		{"type": Particle.WATER, "color": Color.DEEP_SKY_BLUE},
		{"type": Particle.ROCK, "color": Color.DIM_GRAY},
		{"type": Particle.EMPTY, "color": Color.BLACK}
	]
	
	for particle_data in particles:
		var button = ui_buttons[particle_data.type]
		var style = StyleBoxFlat.new()
		
		# Sélectionné : plus clair, sinon couleur normale
		if particle_data.type == selected_particle:
			style.bg_color = particle_data.color.lightened(0.3)
			style.set_border_width_all(3) 
			style.border_color = Color.WHITE
		else:
			style.bg_color = particle_data.color
		
		button.add_theme_stylebox_override("normal", style)

func _process(_delta):
	# Mise à jour de la simulation
	update_grid()
	
	# Rendu
	render_grid()

func update_grid():
	# On met à jour du bas en haut pour éviter de déplacer deux fois la même particule
	for y in range(GRID_HEIGHT - 1, -1, -1):
		# Random la mise à jour de gauche à droite ou de droite à gauche
		var direction = 1 if randf() > 0.5 else -1
		var start = 0 if direction == 1 else GRID_WIDTH - 1
		var end = GRID_WIDTH if direction == 1 else -1
		
		for x in range(start, end, direction):
			match grid[y][x]:
				Particle.SAND:
					move_sand(x, y)
				Particle.WATER:
					move_water(x, y)

func move_sand(x: int, y: int):
	# Le sable tombe tout droit ou en diagonal si possible
	
	# On vérifie si on peut aller vers les bas
	if can_move_to(x, y + 1) and is_empty_or_water(x, y + 1):
		swap_particles(x, y, x, y + 1)
		return
	
	# On choisis au hasard gauche ou droite pour le sable qui tombe
	var try_left_first = randf() > 0.5
	
	if try_left_first:
		if can_move_to(x - 1, y + 1) and is_empty_or_water(x - 1, y + 1):
			swap_particles(x, y, x - 1, y + 1)
			return
		if can_move_to(x + 1, y + 1) and is_empty_or_water(x + 1, y + 1):
			swap_particles(x, y, x + 1, y + 1)
			return
	else:
		if can_move_to(x + 1, y + 1) and is_empty_or_water(x + 1, y + 1):
			swap_particles(x, y, x + 1, y + 1)
			return
		if can_move_to(x - 1, y + 1) and is_empty_or_water(x - 1, y + 1):
			swap_particles(x, y, x - 1, y + 1)
			return

func move_water(x: int, y: int):
	# L'eau tombe vers le bas, se déplace sur les côtés
	
	# Try to move down
	if can_move_to(x, y + 1) and grid[y + 1][x] == Particle.EMPTY:
		swap_particles(x, y, x, y + 1)
		return
	
	# Essais en diagonale
	var try_left_first = randf() > 0.5
	if try_left_first:
		if can_move_to(x - 1, y + 1) and grid[y - 1][x - 1] == Particle.EMPTY:
			swap_particles(x, y, x - 1, y + 1)
			return
		if can_move_to(x + 1, y + 1) and grid[y + 1][x + 1] == Particle.EMPTY:
			swap_particles(x, y, x + 1, y + 1)
			return
	else:
		if can_move_to(x + 1, y + 1) and grid[y + 1][x + 1] == Particle.EMPTY:
			swap_particles(x, y, x + 1, y + 1)
			return
		if can_move_to(x - 1, y + 1) and grid[y + 1][x - 1] == Particle.EMPTY:
			swap_particles(x, y, x - 1, y + 1)
			return
	
	# Test de déplacement horizontal
	var horizontal_dir = 1 if randf() > 0.5 else -1
	if can_move_to(x + horizontal_dir, y) and grid[y][x + horizontal_dir] == Particle.EMPTY:
		swap_particles(x, y, x + horizontal_dir, y)
		return

func can_move_to(x: int, y: int) -> bool:
	# On regarde si on est dans la grille
	return x >= 0 and x < GRID_WIDTH and y >= 0 and y < GRID_HEIGHT

func is_empty_or_water(x: int, y: int) -> bool:
	# Le sable passe à travers l'eau
	return grid[y][x] == Particle.EMPTY or grid[y][x] == Particle.WATER

func swap_particles(x1: int, y1: int, x2: int, y2: int):
	var temp = grid[y1][x1]
	grid[y1][x1] = grid[y2][x2]
	grid[y2][x2] = temp

func try_move_diagonal(x: int, y: int, dir: int) -> bool:
	var new_x = x + dir
	var new_y = y + 1
	
	# On check si la position choisie est vide
	if new_x >= 0 and new_x < GRID_WIDTH and new_y < GRID_HEIGHT:
		if grid[new_y][new_x] == Particle.EMPTY:
			grid[new_y][new_x] = Particle.SAND
			grid[y][x] = Particle.EMPTY
			return true
	
	return false

func render_grid():
	# On convertit notre grille en pixels sur l'image
	for y in range(GRID_HEIGHT):
		for x in range(GRID_WIDTH):
			var color: Color
			match grid[y][x]:
				Particle.EMPTY:
					color = Color.BLACK
				Particle.SAND:
					color = Color.SANDY_BROWN
				Particle.WATER:
					color = Color.DEEP_SKY_BLUE
				Particle.ROCK:
					color = Color.DIM_GRAY
			
			image.set_pixel(x, y, color)
	
	# On met à jour la texture avec les nouveaux pixels
	texture.update(image)

# Pour placer le sable avec la souris - Hardcoded
func _input(event):
	if event is InputEventMouseButton or event is InputEventMouseMotion:
		if event is InputEventMouseButton:
			if event.pressed and event.button_index == MOUSE_BUTTON_LEFT:
				place_particle_at_mouse()
		elif event is InputEventMouseMotion:
			if event.button_mask & MOUSE_BUTTON_MASK_LEFT:
				place_particle_at_mouse()

func place_particle_at_mouse():
	var mouse_pos = get_local_mouse_position()
	var grid_x = int(mouse_pos.x / CELL_SIZE)
	var grid_y = int(mouse_pos.y / CELL_SIZE)
	
	# Vérifie qu'on est dans la grille et pas sur l'ui
	if grid_x >= 0 and grid_x < GRID_WIDTH and grid_y >= 0 and grid_y < GRID_HEIGHT:
		place_particle_area(grid_x, grid_y, 5)

func place_particle_area(center_x: int, center_y: int, radius: int):
	for y in range(center_y - radius, center_y + radius + 1):
		for x in range(center_x - radius, center_x + radius + 1):
			if x >= 0 and x < GRID_WIDTH and y >= 0 and y < GRID_HEIGHT:
				# On place seulement dans le rayon
				var dist = Vector2(x - center_x, y - center_y).length()
				if dist <= radius:
					grid[y][x] = selected_particle
