"""Functional test suite for Blender on Android.

Run by the Android harness through `--python` (see run_functional_tests.sh). Tests run one per
`bpy.app.timers` tick so the UI event loop keeps running: a slow test cannot turn the whole run
into an ANR, and the partial JSON on disk says exactly which test was in flight if the process
dies.

Results:
  /sdcard/Download/Blender/tests/functional/functional.json   (final, "done": true)
  /sdcard/Download/Blender/tests/functional/functional.log    (human readable)
"""

import bpy
import json
import os
import sys
import time
import traceback

ROOT = "/sdcard/Download/Blender/tests/functional"
os.makedirs(ROOT, exist_ok=True)
JSON_PATH = os.path.join(ROOT, "functional.json")
LOG_PATH = os.path.join(ROOT, "functional.log")

STATE = {
    "index": 0,
    "started": time.time(),
    "results": [],
}


class SkipTest(Exception):
    pass


def log(line):
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(LOG_PATH, "a") as handle:
        handle.write("%s %s\n" % (stamp, line))
        handle.flush()


def write_json(done=False):
    payload = {
        "schema": 1,
        "done": bool(done),
        "blender": bpy.app.version_string,
        "started": STATE["started"],
        "updated": time.time(),
        "tests": STATE["results"],
    }
    with open(JSON_PATH, "w") as handle:
        json.dump(payload, handle, indent=2)
        handle.flush()


def active_object():
    obj = bpy.context.view_layer.objects.active
    if obj is None:
        obj = bpy.context.active_object
    if obj is None:
        raise RuntimeError("no active object")
    return obj


def add_cube(name="TestCube"):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    obj.name = name
    return obj


def ensure_cube():
    obj = bpy.data.objects.get("TestCube")
    if obj is None:
        obj = add_cube()
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    return obj


def clear_objects():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)


def file_size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return -1


# ---------------------------------------------------------------------------
# Tests: each returns a detail string on success, raises on failure.
# ---------------------------------------------------------------------------


def test_version():
    return "bpy %s" % bpy.app.version_string


def test_gpu_info():
    import gpu

    info = {
        "renderer": gpu.platform.renderer_get(),
        "vendor": gpu.platform.vendor_get(),
        "version": gpu.platform.version_get(),
        "backend": str(gpu.platform.backend_type_get()),
    }
    return json.dumps(info)


def test_scene():
    scene = bpy.context.scene
    return "scene=%s objects=%d" % (scene.name, len(bpy.data.objects))


def test_add_cube():
    obj = add_cube()
    assert obj.type == "MESH"
    assert len(obj.data.vertices) == 8, len(obj.data.vertices)
    return "verts=%d" % len(obj.data.vertices)


def test_transform():
    obj = ensure_cube()
    obj.location = (1.0, 2.0, 3.0)
    obj.rotation_euler = (0.1, 0.2, 0.3)
    obj.scale = (1.5, 1.5, 1.5)
    bpy.context.view_layer.update()
    loc = tuple(round(v, 3) for v in obj.location)
    assert loc == (1.0, 2.0, 3.0), loc
    return "loc=%s" % (loc,)


def test_edit_extrude():
    obj = ensure_cube()
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    before = len(obj.data.vertices)
    bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0.0, 0.0, 0.5)})
    bpy.ops.object.mode_set(mode="OBJECT")
    after = len(obj.data.vertices)
    assert after > before, (before, after)
    return "verts %d -> %d" % (before, after)


def test_bevel_modifier():
    obj = ensure_cube()
    mod = obj.modifiers.new("BevelTest", "BEVEL")
    mod.width = 0.05
    mod.segments = 2
    bpy.context.view_layer.update()
    assert obj.modifiers.get("BevelTest") is not None
    return "modifier=%s" % mod.type


def test_boolean_modifier():
    obj = ensure_cube()
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.6, location=(0.0, 0.0, 0.0))
    cutter = bpy.context.active_object
    cutter.name = "TestCutter"
    mod = obj.modifiers.new("BoolTest", "BOOLEAN")
    mod.operation = "DIFFERENCE"
    mod.object = cutter
    bpy.context.view_layer.update()
    assert mod.object is cutter
    return "operation=%s" % mod.operation


def test_subsurf_modifier():
    obj = ensure_cube()
    mod = obj.modifiers.new("SubsurfTest", "SUBSURF")
    mod.levels = 2
    mod.render_levels = 2
    bpy.context.view_layer.update()
    assert mod.levels == 2
    return "levels=%d" % mod.levels


def test_array_modifier():
    obj = ensure_cube()
    mod = obj.modifiers.new("ArrayTest", "ARRAY")
    mod.count = 3
    bpy.context.view_layer.update()
    assert mod.count == 3
    return "count=%d" % mod.count


def test_text_object():
    bpy.ops.object.text_add(location=(0.0, 0.0, 2.0))
    obj = bpy.context.active_object
    obj.data.body = "Blender"
    assert obj.type == "FONT"
    return "body=%s" % obj.data.body


def test_curve_object():
    bpy.ops.curve.primitive_bezier_curve_add(location=(0.0, 2.0, 0.0))
    obj = bpy.context.active_object
    assert obj.type == "CURVE"
    return "splines=%d" % len(obj.data.splines)


def test_metaball_object():
    bpy.ops.object.metaball_add(type="BALL", location=(2.0, 0.0, 0.0))
    obj = bpy.context.active_object
    assert obj.type == "META"
    return "elements=%d" % len(obj.data.elements)


def test_armature_object():
    bpy.ops.object.armature_add(location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    assert obj.type == "ARMATURE"
    return "bones=%d" % len(obj.data.bones)


def test_grease_pencil_object():
    try:
        bpy.ops.object.grease_pencil_add(location=(0.0, 0.0, 0.0))
    except Exception:
        bpy.ops.object.gpencil_add(type="EMPTY", location=(0.0, 0.0, 0.0))
    obj = bpy.context.active_object
    assert obj.type in {"GREASEPENCIL", "GPENCIL"}, obj.type
    return "type=%s" % obj.type


def test_uv_unwrap():
    obj = ensure_cube()
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project()
    bpy.ops.object.mode_set(mode="OBJECT")
    assert len(obj.data.uv_layers) > 0
    return "uv_layers=%d" % len(obj.data.uv_layers)


def test_material_principled():
    obj = ensure_cube()
    mat = bpy.data.materials.new("TestMaterial")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    assert bsdf is not None
    bsdf.inputs["Base Color"].default_value = (0.8, 0.2, 0.1, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.35
    obj.data.materials.append(mat)
    assert obj.data.materials[0] == mat
    return "base_color=%s" % (tuple(bsdf.inputs["Base Color"].default_value),)


def test_shader_nodes():
    mat = bpy.data.materials.get("TestMaterial")
    if mat is None:
        mat = bpy.data.materials.new("TestMaterial")
        mat.use_nodes = True
    tree = mat.node_tree
    noise = tree.nodes.new("ShaderNodeTexNoise")
    ramp = tree.nodes.new("ShaderNodeValToRGB")
    bsdf = tree.nodes.get("Principled BSDF")
    tree.links.new(noise.outputs["Fac"], ramp.inputs["Fac"])
    tree.links.new(ramp.outputs["Color"], bsdf.inputs["Base Color"])
    assert len(tree.links) >= 2
    return "nodes=%d links=%d" % (len(tree.nodes), len(tree.links))


def test_image_create_save():
    image = bpy.data.images.new("TestImage", width=64, height=64, alpha=True)
    image.generated_color = (0.2, 0.6, 0.9, 1.0)
    path = os.path.join(ROOT, "test_image.png")
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()
    size = file_size(path)
    assert size > 0, size
    return "png_bytes=%d" % size


def test_world_nodes():
    world = bpy.data.worlds.new("TestWorld")
    world.use_nodes = True
    bg = world.node_tree.nodes.get("Background")
    assert bg is not None
    bg.inputs["Color"].default_value = (0.05, 0.1, 0.2, 1.0)
    bpy.context.scene.world = world
    return "world=%s" % world.name


def test_animation_keyframes():
    obj = ensure_cube()
    obj.animation_data_clear()
    obj.location = (0.0, 0.0, 0.0)
    obj.keyframe_insert(data_path="location", frame=1)
    obj.location = (2.0, 0.0, 0.0)
    obj.keyframe_insert(data_path="location", frame=10)
    action = obj.animation_data.action
    assert action is not None
    curves = len(action.fcurves) if hasattr(action, "fcurves") else len(action.layers)
    return "action=%s curves=%d" % (action.name, curves)


def test_geometry_nodes():
    obj = ensure_cube()
    ng = bpy.data.node_groups.new("TestGeometryNodes", "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    group_in = ng.nodes.new("NodeGroupInput")
    group_out = ng.nodes.new("NodeGroupOutput")
    cube = ng.nodes.new("GeometryNodeMeshCube")
    ng.links.new(cube.outputs["Mesh"], group_out.inputs["Geometry"])
    mod = obj.modifiers.new("GNTest", "NODES")
    mod.node_group = ng
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    assert evaluated.data is not None
    return "node_group=%s" % ng.name


def test_compositor():
    scene = bpy.context.scene
    tree = bpy.data.node_groups.new("TestCompositor", "CompositorNodeTree")
    scene.compositing_node_group = tree
    glare = tree.nodes.new("CompositorNodeGlare")
    assert glare.name in tree.nodes
    nodes = len(tree.nodes)
    # Leave the scene's default compositor in place for the render tests; an empty compositor
    # node group turns every render into a constant background.
    scene.compositing_node_group = None
    return "nodes=%d" % nodes


def test_undo_redo():
    window_manager = bpy.context.window_manager
    if not window_manager.windows:
        raise SkipTest("no UI window in this context")
    window = window_manager.windows[0]
    area = next((area for area in window.screen.areas if area.type == "VIEW_3D"), None)
    if area is None:
        raise SkipTest("no 3D viewport in this context")
    region = next((region for region in area.regions if region.type == "WINDOW"), None)
    override = {"window": window, "screen": window.screen, "area": area}
    if region is not None:
        override["region"] = region
    obj = ensure_cube()
    obj.location = (0.0, 0.0, 0.0)
    obj.location = (5.0, 5.0, 5.0)
    try:
        with bpy.context.temp_override(**override):
            bpy.ops.ed.undo()
    except RuntimeError as exception:
        raise SkipTest("ed.undo needs an interactive context: %s" % exception)
    with bpy.context.temp_override(**override):
        bpy.context.view_layer.update()
        undone = tuple(round(v, 3) for v in bpy.data.objects["TestCube"].location)
        bpy.ops.ed.redo()
        bpy.context.view_layer.update()
        redone = tuple(round(v, 3) for v in bpy.data.objects["TestCube"].location)
    assert undone != (5.0, 5.0, 5.0), undone
    assert redone == (5.0, 5.0, 5.0), (redone, undone)
    return "undo=%s redo=%s" % (undone, redone)


def test_save_blend():
    ensure_cube()
    path = os.path.join(ROOT, "test_scene.blend")
    bpy.ops.wm.save_as_mainfile(filepath=path)
    size = file_size(path)
    assert size > 0, size
    return "blend_bytes=%d" % size


def test_export_obj():
    ensure_cube()
    path = os.path.join(ROOT, "test_export.obj")
    bpy.ops.wm.obj_export(filepath=path, export_selected_objects=False)
    size = file_size(path)
    assert size > 0, size
    return "obj_bytes=%d" % size


def test_import_obj():
    path = os.path.join(ROOT, "test_export.obj")
    if file_size(path) <= 0:
        test_export_obj()
    before = len(bpy.data.objects)
    bpy.ops.wm.obj_import(filepath=path)
    after = len(bpy.data.objects)
    assert after > before, (before, after)
    return "objects %d -> %d" % (before, after)


def test_export_gltf():
    ensure_cube()
    path = os.path.join(ROOT, "test_export.glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB")
    size = file_size(path)
    assert size > 0, size
    return "glb_bytes=%d" % size


def test_export_fbx():
    ensure_cube()
    path = os.path.join(ROOT, "test_export.fbx")
    bpy.ops.export_scene.fbx(filepath=path)
    size = file_size(path)
    assert size > 0, size
    return "fbx_bytes=%d" % size


def test_export_stl():
    ensure_cube()
    path = os.path.join(ROOT, "test_export.stl")
    bpy.ops.wm.stl_export(filepath=path)
    size = file_size(path)
    assert size > 0, size
    return "stl_bytes=%d" % size


def test_addon_toggle():
    import addon_utils

    addon_utils.enable("io_scene_gltf2", default_set=False, handle_error=None)
    enabled = addon_utils.check("io_scene_gltf2")[0]
    addon_utils.disable("io_scene_gltf2", default_set=False, handle_error=None)
    assert enabled
    return "io_scene_gltf2 enabled and disabled"


def test_python_exec():
    namespace = {}
    exec("answer = 6 * 7", namespace)
    assert namespace["answer"] == 42
    return "exec answer=42"


def test_sequencer():
    scene = bpy.context.scene
    scene.sequence_editor_create()
    editor = scene.sequence_editor
    assert editor is not None
    strip = editor.strips.new_effect(
        name="TestColor",
        type="COLOR",
        channel=1,
        frame_start=1,
        length=10,
    )
    assert strip is not None
    assert len(editor.strips_all) >= 1, len(editor.strips_all)
    return "strips=%d" % len(editor.strips_all)


def test_rigidbody():
    obj = ensure_cube()
    bpy.ops.rigidbody.object_add()
    assert obj.rigid_body is not None
    return "type=%s" % obj.rigid_body.type


def ensure_camera_light():
    scene = bpy.context.scene
    cube = ensure_cube()
    camera = scene.camera
    if camera is None:
        bpy.ops.object.camera_add(location=(4.0, -4.0, 3.0))
        camera = bpy.context.active_object
        scene.camera = camera
    direction = cube.location - camera.location
    camera.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    if not any(obj.type == "LIGHT" for obj in scene.objects):
        bpy.ops.object.light_add(type="AREA", location=(3.0, -2.0, 4.0))
        light = bpy.context.active_object
        light.data.energy = 1000.0
    if scene.world is None:
        scene.world = bpy.data.worlds.new("TestRenderWorld")
    return camera


def _render(name, engine, samples):
    scene = bpy.context.scene
    ensure_camera_light()
    # An empty compositor node group turns the render into a constant background. The
    # compositor test leaves one behind; bypass compositing for the render checks.
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.compositing_node_group = None
    scene.render.engine = engine
    scene.render.resolution_x = 192
    scene.render.resolution_y = 144
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    if engine == "CYCLES":
        scene.cycles.device = "CPU"
        scene.cycles.samples = min(samples, 2)
    else:
        scene.eevee.taa_render_samples = min(samples, 2)
    path = os.path.join(ROOT, name + ".png")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    size = file_size(path)
    assert size > 0, size
    image = bpy.data.images.load(path, check_existing=False)
    pixels = image.pixels[:]
    rgb = [pixels[index] for index in range(0, len(pixels), 4)]
    rgb += [pixels[index] for index in range(1, len(pixels), 4)]
    rgb += [pixels[index] for index in range(2, len(pixels), 4)]
    assert max(rgb) - min(rgb) > 0.02, "render is a uniform image"
    return "%s %s bytes=%d" % (engine, name, size)


def test_eevee_render():
    ensure_cube()
    try:
        return _render("test_eevee", "BLENDER_EEVEE_NEXT", 4)
    except TypeError:
        return _render("test_eevee", "BLENDER_EEVEE", 4)


def test_cycles_render():
    ensure_cube()
    return _render("test_cycles", "CYCLES", 4)


def test_render_animation():
    scene = bpy.context.scene
    ensure_cube()
    ensure_camera_light()
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.compositing_node_group = None
    scene.frame_start = 1
    scene.frame_end = 2
    scene.render.resolution_x = 96
    scene.render.resolution_y = 72
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = os.path.join(ROOT, "anim_")
    bpy.ops.render.render(animation=True)
    files = [name for name in os.listdir(ROOT) if name.startswith("anim_") and name.endswith(".png")]
    assert len(files) >= 2, files
    return "frames=%d" % len(files)


def test_memory_read():
    import resource

    usage = resource.getrusage(resource.RUSAGE_SELF)
    return "maxrss_kb=%d" % usage.ru_maxrss


def test_image_reload():
    path = os.path.join(ROOT, "test_image.png")
    if file_size(path) <= 0:
        test_image_create_save()
    image = bpy.data.images.load(path, check_existing=False)
    assert tuple(image.size) == (64, 64), image.size
    image.reload()
    assert tuple(image.size) == (64, 64), image.size
    return "size=%s" % (tuple(image.size),)


def test_image_colorspace():
    image = bpy.data.images.get("TestImage")
    if image is None:
        test_image_reload()
        image = bpy.data.images.get("TestImage")
    if image is None:
        raise SkipTest("no test image")
    image.colorspace_settings.name = "Non-Color"
    assert image.colorspace_settings.name == "Non-Color"
    return "colorspace=%s" % image.colorspace_settings.name


def test_timeline():
    scene = bpy.context.scene
    scene.frame_set(10)
    assert scene.frame_current == 10, scene.frame_current
    scene.frame_set(1)
    assert scene.frame_current == 1
    return "frame_set ok"


def test_pose_bone():
    bpy.ops.object.armature_add(location=(0.0, 0.0, 0.0))
    armature = bpy.context.active_object
    bpy.ops.object.mode_set(mode="POSE")
    bone = armature.pose.bones[0]
    bone.rotation_mode = "XYZ"
    bone.rotation_euler = (0.2, 0.0, 0.0)
    bpy.context.view_layer.update()
    bpy.ops.object.mode_set(mode="OBJECT")
    assert tuple(round(v, 3) for v in armature.pose.bones[0].rotation_euler) == (0.2, 0.0, 0.0)
    return "pose bone rotated"


def test_geometry_scatter():
    obj = ensure_cube()
    ng = bpy.data.node_groups.new("TestScatter", "GeometryNodeTree")
    ng.interface.new_socket("Geometry", in_out="INPUT", socket_type="NodeSocketGeometry")
    ng.interface.new_socket("Geometry", in_out="OUTPUT", socket_type="NodeSocketGeometry")
    group_in = ng.nodes.new("NodeGroupInput")
    group_out = ng.nodes.new("NodeGroupOutput")
    distribute = ng.nodes.new("GeometryNodeDistributePointsOnFaces")
    instance = ng.nodes.new("GeometryNodeInstanceOnPoints")
    cube = ng.nodes.new("GeometryNodeMeshCube")
    ng.links.new(group_in.outputs["Geometry"], distribute.inputs["Mesh"])
    ng.links.new(distribute.outputs["Points"], instance.inputs["Points"])
    ng.links.new(cube.outputs["Mesh"], instance.inputs["Instance"])
    ng.links.new(instance.outputs["Instances"], group_out.inputs["Geometry"])
    mod = obj.modifiers.new("GNScatter", "NODES")
    mod.node_group = ng
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    assert evaluated is not None
    return "nodes=%d" % len(ng.nodes)


def test_append_link():
    path = os.path.join(ROOT, "test_library.blend")
    ensure_cube()
    # copy=True writes the library without making it the current file; loading from the current
    # file is rejected by bpy.data.libraries.load.
    bpy.ops.wm.save_as_mainfile(filepath=path, copy=True)
    with bpy.data.libraries.load(path, link=True) as (data_from, data_to):
        data_to.objects = data_from.objects[:1]
    assert len(data_to.objects) >= 1
    obj = data_to.objects[0]
    assert obj is not None and obj.library is not None, obj
    return "linked=%s" % obj.name


def test_render_formats():
    scene = bpy.context.scene
    ensure_camera_light()
    scene.render.use_compositing = False
    scene.render.use_sequencer = False
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = 96
    scene.render.resolution_y = 72
    scene.render.resolution_percentage = 100
    scene.eevee.taa_render_samples = 1
    results = []
    for file_format, extension in (("JPEG", ".jpg"), ("OPEN_EXR", ".exr")):
        path = os.path.join(ROOT, "test_format" + extension)
        scene.render.image_settings.file_format = file_format
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        size = file_size(path)
        assert size > 0, (file_format, size)
        results.append("%s=%d" % (file_format, size))
    return " ".join(results)


def test_large_grid():
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=300, y_subdivisions=300, size=2.0)
    obj = bpy.context.active_object
    vertices = len(obj.data.vertices)
    assert vertices > 80000, vertices
    depsgraph = bpy.context.evaluated_depsgraph_get()
    obj.evaluated_get(depsgraph)
    return "vertices=%d" % vertices


def test_selection():
    obj = ensure_cube()
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    assert obj.select_get()
    obj.select_set(False)
    assert not obj.select_get()
    return "selection ok"


def test_material_transparency():
    mat = bpy.data.materials.get("TestMaterial")
    if mat is None:
        test_material_principled()
        mat = bpy.data.materials.get("TestMaterial")
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = "DITHERED"
        detail = "surface_render_method=DITHERED"
    else:
        mat.blend_method = "BLEND"
        detail = "blend_method=BLEND"
    return detail


def test_external_script():
    path = os.path.join(ROOT, "external_test.py")
    with open(path, "w") as handle:
        handle.write("value = 21 * 2\n")
    namespace = {}
    with open(path) as handle:
        exec(compile(handle.read(), path, "exec"), namespace)
    assert namespace["value"] == 42
    return "external script value=42"


def test_vse_sound():
    import wave

    path = os.path.join(ROOT, "test_tone.wav")
    with wave.open(path, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(22050)
        handle.writeframes(b"\x00\x00" * 2205)
    scene = bpy.context.scene
    scene.sequence_editor_create()
    editor = scene.sequence_editor
    strip = editor.strips.new_sound(
        name="TestTone",
        filepath=path,
        channel=2,
        frame_start=1,
    )
    assert strip is not None
    assert strip.type == "SOUND", strip.type
    return "sound strip=%s" % strip.name


def test_repeated_renders():
    results = []
    for index in range(5):
        detail = _render("repeat_%d" % index, "BLENDER_EEVEE", 1)
        results.append(detail.split("bytes=")[-1])
    return "5 renders bytes=%s" % ",".join(results)


def test_image_texture_node():
    mat = bpy.data.materials.get("TestMaterial")
    if mat is None:
        test_material_principled()
        mat = bpy.data.materials.get("TestMaterial")
    image = bpy.data.images.get("TestImage")
    if image is None:
        test_image_reload()
        image = bpy.data.images.get("TestImage")
    tree = mat.node_tree
    node = tree.nodes.new("ShaderNodeTexImage")
    node.image = image
    bsdf = tree.nodes.get("Principled BSDF")
    tree.links.new(node.outputs["Color"], bsdf.inputs["Base Color"])
    assert node.image is image
    return "image_node=%s" % image.name


def test_node_group_library():
    path = os.path.join(ROOT, "test_node_library.blend")
    group = bpy.data.node_groups.new("TestLibraryGroup", "GeometryNodeTree")
    group.use_fake_user = True
    # copy=True saves a library without making it the current file.
    bpy.ops.wm.save_as_mainfile(filepath=path, copy=True)
    with bpy.data.libraries.load(path, link=True) as (data_from, data_to):
        data_to.node_groups = data_from.node_groups[:1]
    assert len(data_to.node_groups) >= 1, len(data_from.node_groups)
    linked = data_to.node_groups[0]
    assert linked is not None and linked.library is not None, linked
    return "linked_group=%s" % linked.name


TESTS = [
    ("A13", "Python runtime", test_version),
    ("B01", "GPU info", test_gpu_info),
    ("A14", "Scene state", test_scene),
    ("D01", "Add cube", test_add_cube),
    ("D02", "Transform", test_transform),
    ("D03", "Edit-mode extrude", test_edit_extrude),
    ("D04", "Bevel modifier", test_bevel_modifier),
    ("D05", "Boolean modifier", test_boolean_modifier),
    ("D06", "Subdivision modifier", test_subsurf_modifier),
    ("D07", "Array modifier", test_array_modifier),
    ("D08", "Text object", test_text_object),
    ("D08b", "Curve object", test_curve_object),
    ("D09", "Metaball object", test_metaball_object),
    ("D09b", "Armature object", test_armature_object),
    ("D09c", "Grease pencil object", test_grease_pencil_object),
    ("I01", "UV smart project", test_uv_unwrap),
    ("E01", "Material + Principled", test_material_principled),
    ("E03", "Shader node links", test_shader_nodes),
    ("I03", "Image create/save", test_image_create_save),
    ("E05", "World nodes", test_world_nodes),
    ("G01", "Animation keyframes", test_animation_keyframes),
    ("H01", "Geometry Nodes", test_geometry_nodes),
    ("F05", "Compositor Glare", test_compositor),
    ("D10", "Undo/redo", test_undo_redo),
    ("J01", "Save .blend", test_save_blend),
    ("J04", "Export OBJ", test_export_obj),
    ("J05", "Import OBJ", test_import_obj),
    ("J06", "Export glTF/GLB", test_export_gltf),
    ("J07", "Export FBX", test_export_fbx),
    ("J08", "Export STL", test_export_stl),
    ("K03", "Add-on enable/disable", test_addon_toggle),
    ("K01", "Python exec", test_python_exec),
    ("L04", "Sequencer color strip", test_sequencer),
    ("G06", "Rigid body", test_rigidbody),
    ("F01", "EEVEE render", test_eevee_render),
    ("F02", "Cycles CPU render", test_cycles_render),
    ("F03", "Render animation", test_render_animation),
    ("I04", "Image reload", test_image_reload),
    ("I06", "Image color space", test_image_colorspace),
    ("G02", "Timeline frame set", test_timeline),
    ("G05", "Pose bone", test_pose_bone),
    ("H02", "Geometry scatter", test_geometry_scatter),
    ("J09", "Append/link library", test_append_link),
    ("F04", "JPEG/EXR render", test_render_formats),
    ("M02", "Large grid", test_large_grid),
    ("B06", "Selection", test_selection),
    ("E06", "Material transparency", test_material_transparency),
    ("K02", "External script", test_external_script),
    ("E04", "Image texture node", test_image_texture_node),
    ("H03", "Node group library link", test_node_group_library),
    ("L01", "VSE sound strip", test_vse_sound),
    ("M05", "Five renders", test_repeated_renders),
    ("M0x", "Memory usage", test_memory_read),
]


# A tiny filter file keeps iterative runs short: write IDs, one per line.
ONLY_PATH = "/sdcard/Download/Blender/tests/only.txt"
if os.path.exists(ONLY_PATH):
    with open(ONLY_PATH) as handle:
        only = {token.strip() for token in handle.read().split()}
    if only:
        TESTS = [test for test in TESTS if test[0] in only]


def record(test_id, name, status, detail, elapsed_ms):
    result = {
        "id": test_id,
        "name": name,
        "status": status,
        "detail": detail,
        "ms": int(elapsed_ms),
    }
    STATE["results"].append(result)
    log("%s %-8s %s — %s" % (test_id, status, name, detail.replace("\n", " ")[:300]))
    write_json(done=False)


def tick():
    if STATE["index"] >= len(TESTS):
        summary = {}
        for result in STATE["results"]:
            summary[result["status"]] = summary.get(result["status"], 0) + 1
        log("DONE %s" % json.dumps(summary, sort_keys=True))
        write_json(done=True)
        return None

    test_id, name, fn = TESTS[STATE["index"]]
    STATE["index"] += 1
    start = time.time()
    try:
        detail = fn()
        status = "PASS"
    except SkipTest as exception:
        status = "SKIP"
        detail = str(exception)
    except Exception as exception:
        status = "FAIL"
        detail = "%s: %s\n%s" % (
            type(exception).__name__,
            exception,
            traceback.format_exc(limit=6),
        )
    record(test_id, name, status, str(detail), (time.time() - start) * 1000.0)
    return 0.02


log("START blender=%s python=%s" % (bpy.app.version_string, sys.version.replace("\n", " ")))
write_json(done=False)
bpy.app.timers.register(tick, first_interval=0.5)
