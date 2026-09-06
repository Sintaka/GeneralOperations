#!/usr/bin/env python3
r"""
@name        PMX to FBX
@group       Geometry
@desc        用 Blender + mmd_tools 将 MMD PMX 模型转换为 FBX，保留材质/贴图/形态键/骨架
@accepts     file
@ext         .pmx
@multi       false
@host        blender
@blender     C:/Program Files/Blender 5.2/blender.exe

Usage:
    blender.exe --background --python geo.pmx2fbx.py -- path\to\model.pmx
"""

import bpy, sys, os, traceback, pathlib, importlib, subprocess

# ---------------------------------------------------------------------------
# Resolve mmd_tools
# ---------------------------------------------------------------------------

def _mmd_dir():
    appdata = os.getenv("APPDATA") or os.path.expanduser("~")
    v = f"{bpy.app.version[0]}.{bpy.app.version[1]}"
    for root in (
        pathlib.Path(appdata) / "Blender Foundation" / "Blender" / v / "extensions",
        pathlib.Path(appdata) / "Blender Foundation" / "Blender" / v / "scripts" / "addons",
    ):
        if not root.is_dir():
            continue
        for d in root.rglob("mmd_tools"):
            if d.is_dir() and (d / "__init__.py").exists():
                return d
    return None

def _install_wheels(pkg_dir):
    import tomllib
    mf = pkg_dir / "blender_manifest.toml"
    if not mf.is_file():
        return
    cfg = tomllib.loads(mf.read_text())
    for w in cfg.get("wheels", []):
        whl = (pkg_dir / w).resolve()
        if not whl.is_file():
            continue
        pkg = whl.name.partition("-")[0].replace("_", "-").lower()
        try:
            importlib.import_module(pkg.replace("-", "_"))
        except ModuleNotFoundError:
            subprocess.run([sys.executable, "-m", "pip", "install", str(whl)],
                           capture_output=True, text=True, timeout=120,
                           env={**os.environ, "PIP_DISABLE_PIP_VERSION_CHECK": "1"})

def enable_mmd_tools():
    pkg_dir = _mmd_dir()
    if pkg_dir:
        _install_wheels(pkg_dir)
    for name in list(bpy.context.preferences.addons.keys()):
        if "mmd_tools" in name.lower():
            try:
                bpy.context.preferences.addons[name].enabled = True
            except Exception:
                pass
            return True
    for nm in ("bl_ext.blender_org.mmd_tools", "mmd_tools"):
        try:
            bpy.ops.preferences.addon_enable(module=nm)
            for r in bpy.context.preferences.addons.keys():
                if "mmd_tools" in r.lower():
                    return True
        except Exception:
            pass
    if not pkg_dir:
        return False
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "mmd_tools", str(pkg_dir / "__init__.py"),
        submodule_search_locations=[str(pkg_dir)])
    if spec is None or spec.loader is None:
        return False
    mod = importlib.util.module_from_spec(spec)
    sys.modules["mmd_tools"] = mod
    spec.loader.exec_module(mod)
    if hasattr(mod, "register"):
        mod.register()
    return True


# ---------------------------------------------------------------------------
# Main pipeline
# ---------------------------------------------------------------------------

def main():
    # Blender mixes its own arguments into sys.argv, so only what follows
    # the literal "--" separator belongs to this script.
    argv = sys.argv
    if '--' in argv:
        argv = argv[argv.index('--') + 1:]
    else:
        argv = []
    if not argv:
        print("ERROR: No PMX file provided.")
        sys.exit(1)

    pmx_path = os.path.abspath(argv[0])
    if not os.path.isfile(pmx_path):
        print(f"ERROR: File not found: {pmx_path}")
        sys.exit(1)

    fbx_path = os.path.splitext(pmx_path)[0] + '.fbx'

    print("=" * 60)
    print("  PMX -> FBX Converter")
    print("=" * 60)
    print(f"  Python  : {sys.version.split()[0]}")
    print(f"  Blender : {bpy.app.version_string}")
    print(f"  OCIO    : {os.getenv('OCIO', '(unset)')}")
    print(f"  Input   : {pmx_path}")
    print(f"  Output  : {fbx_path}")
    print()

    # 1) mmd_tools
    print("[1/6] Enable mmd_tools ...")
    if not enable_mmd_tools():
        print("ERROR: mmd_tools not found")
        sys.exit(2)
    print("  OK\n")

    # 2) Clear scene
    print("[2/6] Clear scene ...")
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    print("  OK\n")

    # 3) Import PMX
    print("[3/6] Import PMX ...")
    try:
        bpy.ops.mmd_tools.import_model(
            filepath=pmx_path, scale=1.0,
            types={'MESH', 'ARMATURE', 'MORPHS'})
    except TypeError:
        bpy.ops.mmd_tools.import_model(filepath=pmx_path)

    meshes    = [o for o in bpy.context.scene.objects if o.type == 'MESH']
    armatures = [o for o in bpy.context.scene.objects if o.type == 'ARMATURE']
    shapes    = sum(len(m.data.shape_keys.key_blocks) if m.data.shape_keys else 0
                    for m in meshes)
    print(f"  {len(meshes)} mesh(es), {len(armatures)} armature(s)")
    print(f"  {shapes} shape key(s), {len(bpy.data.materials)} material(s)")
    print("  OK\n")

    # 4) Fix texture paths
    print("[4/6] Fix texture paths ...")
    pmx_dir = pathlib.Path(pmx_path).parent
    tex_dir = pmx_dir / "textures"
    for img in bpy.data.images:
        raw = img.filepath
        if not raw or not raw.strip():
            continue
        cur = pathlib.Path(bpy.path.abspath(raw))
        if cur.is_file():
            continue
        name = cur.name
        if name in ('', '.'):
            continue
        for fdir in (tex_dir, pmx_dir):
            if not fdir.is_dir():
                continue
            for f in fdir.iterdir():
                if f.name.lower() == name.lower() and f.is_file():
                    img.filepath = str(f)
                    break
    print("  OK\n")

    # 5) Rebuild materials as Principled BSDF for FBX compatibility
    print("[5/6] Rebuild materials for FBX ...")

    def _wire_material(mat):
        tree = mat.node_tree
        if tree is None:
            return
        for n in tree.nodes:
            if n.type == 'BSDF_PRINCIPLED':
                for out_n in tree.nodes:
                    if out_n.type == 'OUTPUT_MATERIAL':
                        if out_n.inputs['Surface'].is_linked:
                            src = out_n.inputs['Surface'].links[0].from_node
                            if src.type == 'BSDF_PRINCIPLED':
                                return
        tex_nodes = [(n, n.image) for n in tree.nodes
                     if n.type == 'TEX_IMAGE' and n.image]
        if not tex_nodes:
            return
        nodes = tree.nodes
        links = tree.links
        nodes.clear()
        for x, (_, img) in enumerate(tex_nodes):
            tex = nodes.new('ShaderNodeTexImage')
            tex.image = img
            tex.location = (-300, -200 * x)
            bsdf = nodes.new('ShaderNodeBsdfPrincipled')
            bsdf.location = (0, 0)
            out = nodes.new('ShaderNodeOutputMaterial')
            out.location = (300, 0)
            links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
            if img.alpha_mode != 'NONE' or img.depth in (32, 128):
                links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
            links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])

    for mat in bpy.data.materials:
        _wire_material(mat)

    ok = sum(1 for m in bpy.data.materials
             if m.node_tree and any(
                 n.type == 'BSDF_PRINCIPLED' for n in m.node_tree.nodes))
    print(f"  {ok}/{len(bpy.data.materials)} materials ready")
    print("  OK\n")

    # 6) Export FBX
    print("[6/6] Export FBX ...")
    bpy.ops.object.select_all(action='SELECT')
    if bpy.context.selected_objects:
        bpy.context.view_layer.objects.active = bpy.context.selected_objects[0]

    bpy.ops.export_scene.fbx(
        filepath=fbx_path,
        use_selection=True,
        global_scale=1.0,
        apply_unit_scale=True,
        apply_scale_options='FBX_SCALE_NONE',
        bake_space_transform=False,
        object_types={'ARMATURE', 'MESH'},
        use_armature_deform_only=False,
        add_leaf_bones=False,
        primary_bone_axis='Y',
        secondary_bone_axis='X',
        bake_anim=False,
        use_mesh_modifiers=True,
        use_mesh_modifiers_render=True,
        mesh_smooth_type='FACE',
        path_mode='COPY',
        embed_textures=True,
        axis_forward='-Z',
        axis_up='Y',
    )

    print(f"  {os.path.getsize(fbx_path) / 1024:,.0f} KB")
    print("=" * 60)
    print("  DONE")
    print("=" * 60)


if __name__ == '__main__':
    try:
        main()
    except SystemExit:
        raise
    except Exception:
        traceback.print_exc()
        print("=" * 60)
        print("  CONVERSION FAILED")
        print("=" * 60)
        sys.exit(1)
