"""Long-running industrial-style workflow for the Android port.

Runs through a production-ish loop (model -> edit -> UV -> material -> animation -> geometry
nodes -> render -> file I/O -> cleanup) for a configurable duration, writing a heartbeat JSON and
log. The ADB monitor injects keyboard/mouse/stylus/touchpad events while this runs, so the app sees
a realistic mix of API work and external input.

Duration comes from /sdcard/Download/Blender/tests/soak_duration.txt (seconds); default 2700.
"""

import bpy
import json
import os
import resource
import time
import traceback

ROOT = "/sdcard/Download/Blender/soak"
os.makedirs(ROOT, exist_ok=True)
JSON_PATH = os.path.join(ROOT, "soak.json")
LOG_PATH = os.path.join(ROOT, "soak.log")
DURATION_PATH = "/sdcard/Download/Blender/tests/soak_duration.txt"
try:
    with open(DURATION_PATH) as handle:
        DURATION = int(handle.read().strip())
except Exception:
    DURATION = 2700

STATE = {
    "start": time.time(),
    "cycle": 0,
    "ops": 0,
    "errors": [],
    "last_step": "",
    "renders": 0,
    "saves": 0,
}


def log(message):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(LOG_PATH, "a") as handle:
        handle.write("%s %s\n" % (stamp, message))
        handle.flush()


def write_json(done=False):
    payload = {
        "schema": 1,
        "done": bool(done),
        "duration_s": DURATION,
        "elapsed_s": round(time.time() - STATE["start"], 1),
        "cycle": STATE["cycle"],
        "ops": STATE["ops"],
        "renders": STATE["renders"],
        "saves": STATE["saves"],
        "maxrss_kb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        "objects": len(bpy.data.objects),
        "last_step": STATE["last_step"],
        "errors": STATE["errors"][-30:],
    }
    with open(JSON_PATH, "w") as handle:
        json.dump(payload, handle, indent=2)
        handle.flush()


def note_error(step, exception):
    entry = "%s: %s: %s" % (step, type(exception).__name__, exception)
    STATE["errors"].append(entry)
    log("ERROR %s" % entry)


def object_active(obj):
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def ensure_cube(name="SoakCube"):
    obj = bpy.data.objects.get(name)
    if obj is None or obj.type != "MESH":
        bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.0))
        obj = bpy.context.active_object
        obj.name = name
    object_active(obj)
    return obj


def ensure_camera_light():
    scene = bpy.context.scene
    cube = ensure_cube()
    camera = scene.camera
    if camera is None or camera.type != "CAMERA":
        bpy.ops.object.camera_add(location=(4.0, -4.0, 3.0))
        camera = bpy.context.active_object
        scene.camera = camera
    direction = cube.location - camera.location
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    if not any(obj.type == "LIGHT" for obj in scene.objects):
        bpy.ops.object.light_add(type="AREA", location=(3.0, -2.0, 4.0))
        light = bpy.context.active_object
        light.data.energy = 1200.0
    return camera


def clear_objects(keep_camera_light=False):
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for _ in range(3):
        try:
            bpy.data.orphans_purge(do_local_ids=True, do_linked_ids=True, do_recursive=True)
        except Exception:
            break


def check_image(path, expect_variation=True):
    size = os.path.getsize(path)
    assert size > 0, path
    if not expect_variation:
        return size
    image = bpy.data.images.load(path, check_existing=False)
    pixels = image.pixels[:]
    rgb = [pixels[i] for i in range(0, len(pixels), 4)]
    rgb += [pixels[i] for i in range(1, len(pixels), 4)]
    rgb += [pixels[i] for i in range(2, len(pixels), 4)]
    if max(rgb) - min(rgb) < 0.01:
        raise RuntimeError("uniform render: %s" % path)
    return size


# ---------------------------------------------------------------------------
# Workflow steps
# ---------------------------------------------------------------------------


def step_model():
    clear_objects()
    obj = ensure_cube()
    obj.location = (0.0, 0.0, 0.0)
    obj.rotation_euler = (0.1, 0.2, 0.3)
    obj.scale = (1.2, 1.0, 0.8)
    bevel = obj.modifiers.new("SoakBevel", "BEVEL")
    bevel.width = 0.04
    bevel.segments = 2
    object_active(obj)
    bpy.ops.object.modifier_apply(modifier=bevel.name)
    return "verts=%d" % len(obj.data.vertices)


def step_edit():
    obj = ensure_cube()
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0.0, 0.0, 0.3)})
    bpy.ops.mesh.bevel(offset=0.02, segments=2, affect="EDGES")
    bpy.ops.object.mode_set(mode="OBJECT")
    return "verts=%d" % len(obj.data.vertices)


def step_boolean():
    clear_objects()
    cube = ensure_cube()
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.55, location=(0.2, 0.1, 0.0))
    sphere = bpy.context.active_object
    mod = cube.modifiers.new("SoakBoolean", "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.object = sphere
    object_active(cube)
    bpy.ops.object.modifier_apply(modifier=mod.name)
    sphere.hide_set(True)
    return "faces=%d" % len(cube.data.polygons)


def step_subsurf_array():
    clear_objects()
    obj = ensure_cube()
    sub = obj.modifiers.new("SoakSubsurf", "SUBSURF")
    sub.levels = 2
    arr = obj.modifiers.new("SoakArray", "ARRAY")
    arr.count = 3
    arr.relative_offset_displace[0] = 1.4
    return "modifiers=%d" % len(obj.modifiers)


def step_uv():
    obj = ensure_cube()
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")
    return "uv_layers=%d" % len(obj.data.uv_layers)


def step_material():
    obj = ensure_cube()
    mat = bpy.data.materials.get("SoakMaterial")
    if mat is None:
        mat = bpy.data.materials.new("SoakMaterial")
        mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    bsdf.inputs["Base Color"].default_value = (0.25, 0.55, 0.85, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.35
    if not obj.data.materials:
        obj.data.materials.append(mat)
    else:
        obj.data.materials[0] = mat
    noise = mat.node_tree.nodes.get("SoakNoise")
    if noise is None:
        noise = mat.node_tree.nodes.new("ShaderNodeTexNoise")
        noise.name = "SoakNoise"
        ramp = mat.node_tree.nodes.new("ShaderNodeValToRGB")
        ramp.name = "SoakRamp"
        mat.node_tree.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
        mat.node_tree.links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    return "material=%s" % mat.name


def step_image_texture():
    obj = ensure_cube()
    mat = bpy.data.materials.get("SoakMaterial")
    if mat is None:
        step_material()
        mat = bpy.data.materials["SoakMaterial"]
    image = bpy.data.images.get("SoakImage")
    if image is None:
        image = bpy.data.images.new("SoakImage", width=64, height=64, alpha=True)
        image.generated_color = (0.1, 0.7, 0.3, 1.0)
    node = mat.node_tree.nodes.get("SoakImageNode")
    if node is None:
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.name = "SoakImageNode"
        node.image = image
    return "image=%s" % image.name


def step_animation():
    obj = ensure_cube()
    obj.animation_data_clear()
    obj.location = (0.0, 0.0, 0.0)
    obj.keyframe_insert(data_path="location", frame=1)
    obj.location = (1.5, 0.0, 0.0)
    obj.keyframe_insert(data_path="location", frame=20)
    bpy.context.scene.frame_set(10)
    return "action=%s" % (obj.animation_data.action.name if obj.animation_data else "none")


def step_geometry_nodes():
    clear_objects()
    obj = ensure_cube()
    ng = bpy.data.node_groups.get("SoakGeometry")
    if ng is None:
        ng = bpy.data.node_groups.new("SoakGeometry", "GeometryNodeTree")
        ng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
        ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
        group_in = ng.nodes.new("NodeGroupInput")
        group_out = ng.nodes.new("NodeGroupOutput")
        cube = ng.nodes.new("GeometryNodeMeshCube")
        ng.links.new(cube.outputs["Mesh"], group_out.inputs["Geometry"])
    mod = obj.modifiers.new("SoakGN", "NODES")
    mod.node_group = ng
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj.evaluated_get(depsgraph)
    return "nodes=%d" % len(ng.nodes)


def step_rigidbody():
    obj = ensure_cube()
    bpy.ops.rigidbody.object_add()
    return "type=%s" % (obj.rigid_body.type if obj.rigid_body else "none")


def step_large_grid():
    clear_objects()
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=120, y_subdivisions=120, size=2.0)
    obj = bpy.context.active_object
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj.evaluated_get(depsgraph)
    return "vertices=%d" % len(obj.data.vertices)


def step_save_export():
    ensure_cube()
    blend = os.path.join(ROOT, "soak_scene.blend")
    bpy.ops.wm.save_as_mainfile(filepath=blend, copy=True)
    STATE["saves"] += 1
    obj = os.path.join(ROOT, "soak.obj")
    bpy.ops.wm.obj_export(filepath=obj, export_selected_objects=False)
    glb = os.path.join(ROOT, "soak.glb")
    bpy.ops.export_scene.gltf(filepath=glb, export_format="GLB")
    stl = os.path.join(ROOT, "soak.stl")
    bpy.ops.wm.stl_export(filepath=stl)
    sizes = [os.path.getsize(path) for path in (blend, obj, glb, stl)]
    return "bytes=%s" % ",".join(str(size) for size in sizes)


def step_import():
    path = os.path.join(ROOT, "soak.obj")
    if not os.path.exists(path):
        step_save_export()
    before = len(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=path)
    after = len(bpy.data.objects)
    assert after > before
    return "objects=%d" % after


def step_render(engine):
    clear_objects()
    ensure_camera_light()
    scene = bpy.context.scene
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.render.film_transparent = False
    scene.render.engine = engine
    scene.render.resolution_x = 160
    scene.render.resolution_y = 120
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    if engine == "CYCLES":
        scene.render.resolution_x = 128
        scene.render.resolution_y = 96
        scene.cycles.device = "CPU"
        scene.cycles.samples = 1
    else:
        scene.eevee.taa_render_samples = 2
    path = os.path.join(ROOT, "render_%s.png" % engine.lower())
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    size = check_image(path)
    STATE["renders"] += 1
    return "%s=%d" % (engine, size)


def step_vse():
    scene = bpy.context.scene
    scene.sequence_editor_create()
    editor = scene.sequence_editor
    if not any(strip.type == "COLOR" for strip in editor.strips):
        editor.strips.new_effect(name="SoakColor", type="COLOR", channel=1, frame_start=1, length=20)
    tone = "/sdcard/Download/Blender/tests/functional/test_tone.wav"
    if os.path.exists(tone) and not any(strip.type == "SOUND" for strip in editor.strips):
        editor.strips.new_sound(name="SoakTone", filepath=tone, channel=2, frame_start=1)
    scene.render.use_sequencer = False
    return "strips=%d" % len(editor.strips)


def step_python():
    namespace = {}
    exec("soak_value = 6 * 7", namespace)
    assert namespace["soak_value"] == 42
    return "exec=42"


def step_cleanup():
    clear_objects()
    for material in list(bpy.data.materials):
        if material.users == 0:
            bpy.data.materials.remove(material)
    return "objects=%d" % len(bpy.data.objects)


STEPS = [
    ("model", step_model),
    ("edit", step_edit),
    ("uv", step_uv),
    ("material", step_material),
    ("image", step_image_texture),
    ("animation", step_animation),
    ("boolean", step_boolean),
    ("subsurf_array", step_subsurf_array),
    ("geometry", step_geometry_nodes),
    ("rigidbody", step_rigidbody),
    ("grid", step_large_grid),
    ("save_export", step_save_export),
    ("import", step_import),
    ("vse", step_vse),
    ("python", step_python),
    ("render_eevee", lambda: step_render("BLENDER_EEVEE")),
    ("cleanup", step_cleanup),
    ("render_cycles", lambda: step_render("CYCLES")),
]


def tick():
    elapsed = time.time() - STATE["start"]
    if elapsed >= DURATION:
        STATE["last_step"] = "done"
        log("DONE elapsed=%.1f cycles=%d ops=%d errors=%d" % (
            elapsed, STATE["cycle"], STATE["ops"], len(STATE["errors"])))
        write_json(done=True)
        return None

    index = STATE["cycle"] % len(STEPS)
    name, function = STEPS[index]
    STATE["last_step"] = name
    start = time.time()
    try:
        detail = function()
    except Exception as exception:
        note_error(name, exception)
        detail = "ERROR"
    STATE["cycle"] += 1
    STATE["ops"] += 1
    log("cycle=%d step=%s ms=%d detail=%s" % (
        STATE["cycle"], name, int((time.time() - start) * 1000), str(detail)[:160]))
    write_json(done=False)

    # Keep the loop responsive: one step per tick, never a tight loop.
    return 1.0


log("START duration=%ds" % DURATION)
write_json(done=False)
bpy.app.timers.register(tick, first_interval=2.0)
