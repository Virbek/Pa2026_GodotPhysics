extends Node

## Attach this node next to the simulation and assign `target` in the inspector.
## For a fair comparison, expose the same three methods in both implementations:
## get_last_simulation_ms(), get_last_render_ms(), get_active_particle_count().

@export var target: Node
@export var implementation_name := "C++ GDExtension"
@export var warmup_seconds := 3.0
@export var measurement_seconds := 30.0
@export var output_path := "user://falling_sand_benchmark.csv"

var _elapsed := 0.0
var _collecting := false
var _finished := false
var _rows: Array[String] = []

func _ready() -> void:
    _rows.append("implementation,time_s,simulation_ms,render_ms,fps,active_particles")


func _physics_process(delta: float) -> void:
    if _finished or target == null:
        return

    _elapsed += delta

    if not _collecting:
        if _elapsed >= warmup_seconds:
            _collecting = true
            _elapsed = 0.0
        return

    _rows.append(
        "%s,%.4f,%.6f,%.6f,%.2f,%d" % [
            implementation_name,
            _elapsed,
            target.get_last_simulation_ms(),
            target.get_last_render_ms(),
            Engine.get_frames_per_second(),
            target.get_active_particle_count()
        ]
    )

    if _elapsed >= measurement_seconds:
        _write_results()
        _finished = true


func _write_results() -> void:
    var file := FileAccess.open(output_path, FileAccess.WRITE)
    if file == null:
        push_error("Impossible d'écrire le benchmark dans %s" % output_path)
        return

    for row in _rows:
        file.store_line(row)

    file.close()
    print("Benchmark terminé : ", ProjectSettings.globalize_path(output_path))
