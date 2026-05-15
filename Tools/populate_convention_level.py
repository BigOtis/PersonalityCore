import math
import unreal


LEVEL_PATH = "/Game/Level"
TALKER_ASSET_PATH = "/Game/Talker"
FLOOR_SURFACE_Z = 0.0
BOT_FLOOR_PADDING = 4.0


def log(msg):
    unreal.log(f"[populate_convention_level] {msg}")


def load_asset(path):
    asset = unreal.EditorAssetLibrary.load_asset(path)
    if not asset:
        raise RuntimeError(f"Failed to load asset: {path}")
    return asset


def set_prop(obj, value, *names):
    for name in names:
        try:
            obj.set_editor_property(name, value)
            return
        except Exception:
            pass
    raise RuntimeError(f"Unable to set property on {obj}: {names}")


def try_set_prop(obj, value, *names):
    for name in names:
        try:
            obj.set_editor_property(name, value)
            return True
        except Exception:
            pass
    return False


def yaw_vectors(yaw_degrees):
    radians = math.radians(yaw_degrees)
    forward = unreal.Vector(math.cos(radians), math.sin(radians), 0.0)
    right = unreal.Vector(-math.sin(radians), math.cos(radians), 0.0)
    return forward, right


def local_to_world(origin, yaw_degrees, x=0.0, y=0.0, z=0.0):
    forward, right = yaw_vectors(yaw_degrees)
    return unreal.Vector(
        origin.x + forward.x * x + right.x * y,
        origin.y + forward.y * x + right.y * y,
        origin.z + z,
    )


def get_actor_subsystem():
    return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)


def get_level_subsystem():
    return unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)


def get_all_actors():
    return get_actor_subsystem().get_all_level_actors()


def align_actor_to_floor(actor, floor_z=FLOOR_SURFACE_Z, padding=BOT_FLOOR_PADDING):
    origin, extent = actor.get_actor_bounds(False)
    bottom_z = origin.z - extent.z
    delta_z = floor_z + padding - bottom_z
    location = actor.get_actor_location()
    actor.set_actor_location(unreal.Vector(location.x, location.y, location.z + delta_z), False, False)


def cleanup_level():
    actor_subsystem = get_actor_subsystem()
    prefixes = (
        "Convention_",
        "Vendor_",
        "Expo_",
        "Booth_",
    )
    exact_names = {"Talker", "Talker_2", "Talker_3"}

    actors_to_delete = []
    for actor in get_all_actors():
        label = actor.get_actor_label()
        if label in exact_names or label.startswith(prefixes):
            actors_to_delete.append(actor)

    deleted = 0
    for actor in actors_to_delete:
        actor_subsystem.destroy_actor(actor)
        deleted += 1

    log(f"deleted_existing_actors={deleted}")


def spawn_static_mesh(label, mesh, location, rotation, scale, material=None, folder="Convention"):
    log(f"spawn_static_mesh label={label}")
    actor = unreal.EditorLevelLibrary.spawn_actor_from_object(mesh, location, rotation)
    actor.set_actor_label(label)
    actor.set_folder_path(folder)
    actor.set_actor_scale3d(scale)
    smc = actor.get_component_by_class(unreal.StaticMeshComponent)
    if material:
        material_count = max(1, smc.get_num_materials())
        for idx in range(material_count):
            smc.set_material(idx, material)
    return actor


def spawn_text(label, text, location, rotation, scale, color, horizontal="CENTER", folder="Convention"):
    log(f"spawn_text label={label}")
    actor = get_actor_subsystem().spawn_actor_from_class(unreal.TextRenderActor, location, rotation)
    actor.set_actor_label(label)
    actor.set_folder_path(folder)
    actor.set_actor_scale3d(scale)
    comp = actor.get_component_by_class(unreal.TextRenderComponent)
    comp.set_text(text)
    alignment_map = {
        "LEFT": unreal.HorizTextAligment.EHTA_LEFT,
        "CENTER": unreal.HorizTextAligment.EHTA_CENTER,
        "RIGHT": unreal.HorizTextAligment.EHTA_RIGHT,
    }
    comp.set_horizontal_alignment(alignment_map.get(horizontal, unreal.HorizTextAligment.EHTA_CENTER))
    comp.set_text_render_color(color)
    comp.set_world_size(120.0)
    return actor


def configure_world():
    level_subsystem = get_level_subsystem()
    level_subsystem.load_level(LEVEL_PATH)

    for actor in get_all_actors():
        if actor.get_class().get_name() == "PlayerStart":
            actor.set_actor_location(unreal.Vector(-2600.0, 0.0, 120.0), False, False)
            actor.set_actor_rotation(unreal.Rotator(0.0, 0.0, 0.0), False)
            log("player_start_repositioned")
            break


def create_main_stage(mesh_cube, mesh_sphere, mesh_cylinder, materials):
    floor_material = materials["floor"]
    wall_material = materials["wall"]
    accent_material = materials["accent"]
    display_material = materials["display"]
    glass_material = materials["glass"]

    spawn_static_mesh(
        "Convention_Floor",
        mesh_cube,
        unreal.Vector(1150.0, 0.0, -20.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(64.0, 42.0, 0.4),
        floor_material,
    )
    spawn_static_mesh(
        "Convention_BackWall",
        mesh_cube,
        unreal.Vector(3850.0, 0.0, 300.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(0.45, 42.0, 5.2),
        wall_material,
    )
    spawn_static_mesh(
        "Convention_LeftFrame",
        mesh_cube,
        unreal.Vector(1150.0, -2100.0, 150.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(64.0, 0.3, 1.5),
        wall_material,
    )
    spawn_static_mesh(
        "Convention_RightFrame",
        mesh_cube,
        unreal.Vector(1150.0, 2100.0, 150.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(64.0, 0.3, 1.5),
        wall_material,
    )
    spawn_static_mesh(
        "Convention_EntryArchTop",
        mesh_cube,
        unreal.Vector(-2150.0, 0.0, 500.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(0.5, 8.0, 0.45),
        accent_material,
    )
    spawn_static_mesh(
        "Convention_EntryArchLeft",
        mesh_cube,
        unreal.Vector(-2150.0, -780.0, 240.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(0.5, 0.45, 4.8),
        accent_material,
    )
    spawn_static_mesh(
        "Convention_EntryArchRight",
        mesh_cube,
        unreal.Vector(-2150.0, 780.0, 240.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(0.5, 0.45, 4.8),
        accent_material,
    )
    spawn_static_mesh(
        "Convention_StageOrb",
        mesh_sphere,
        unreal.Vector(1550.0, 0.0, 170.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(1.5, 1.5, 1.5),
        glass_material,
    )
    spawn_static_mesh(
        "Convention_StageColumn",
        mesh_cylinder,
        unreal.Vector(1550.0, 0.0, 75.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(1.2, 1.2, 1.2),
        display_material,
    )
    spawn_text(
        "Convention_Title",
        "ASTRA SCIENCE EXPO",
        unreal.Vector(-2050.0, 0.0, 545.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(2.3, 2.3, 2.3),
        unreal.Color(120, 240, 255, 255),
    )
    spawn_text(
        "Convention_Subtitle",
        "Robotics - Materials - Orbital Agriculture - Deep Space Tools",
        unreal.Vector(-1350.0, 0.0, 90.0),
        unreal.Rotator(0.0, 0.0, 0.0),
        unreal.Vector(1.3, 1.3, 1.3),
        unreal.Color(240, 240, 255, 255),
    )


def create_lounge(mesh_table, mesh_chair, mesh_sphere, materials):
    positions = [
        unreal.Vector(550.0, -420.0, 0.0),
        unreal.Vector(2200.0, 420.0, 0.0),
    ]

    for index, center in enumerate(positions, start=1):
        spawn_static_mesh(
            f"Convention_LoungeTable_{index}",
            mesh_table,
            center + unreal.Vector(0.0, 0.0, 0.0),
            unreal.Rotator(0.0, 0.0, 0.0),
            unreal.Vector(1.0, 1.0, 1.0),
            materials["metal"],
        )
        spawn_static_mesh(
            f"Convention_LoungeDisplay_{index}",
            mesh_sphere,
            center + unreal.Vector(0.0, 0.0, 110.0),
            unreal.Rotator(0.0, 0.0, 0.0),
            unreal.Vector(0.65, 0.65, 0.65),
            materials["display"],
        )

        chair_offsets = [
            unreal.Vector(160.0, 0.0, 0.0),
            unreal.Vector(-160.0, 0.0, 0.0),
            unreal.Vector(0.0, 160.0, 0.0),
            unreal.Vector(0.0, -160.0, 0.0),
        ]
        chair_yaws = [180.0, 0.0, -90.0, 90.0]
        for chair_index, (offset, yaw) in enumerate(zip(chair_offsets, chair_yaws), start=1):
            spawn_static_mesh(
                f"Convention_LoungeChair_{index}_{chair_index}",
                mesh_chair,
                center + offset,
                unreal.Rotator(0.0, yaw, 0.0),
                unreal.Vector(1.0, 1.0, 1.0),
                materials["metal"],
            )


def create_booth(name, origin, yaw, material, accent_color, materials, assets):
    mesh_cube = assets["cube"]
    mesh_sphere = assets["sphere"]
    mesh_cylinder = assets["cylinder"]

    piece_defs = [
        ("BackPanel", -220.0, 0.0, 240.0, unreal.Vector(0.7, 8.0, 4.8), material),
        ("WingLeft", -220.0, -400.0, 200.0, unreal.Vector(0.7, 0.5, 3.9), materials["wall"]),
        ("WingRight", -220.0, 400.0, 200.0, unreal.Vector(0.7, 0.5, 3.9), materials["wall"]),
        ("Counter", 40.0, 0.0, 85.0, unreal.Vector(3.6, 1.3, 1.1), materials["metal"]),
        ("AccentBar", -10.0, 0.0, 165.0, unreal.Vector(3.8, 0.25, 0.2), materials["accent"]),
        ("Podium", -60.0, 0.0, 90.0, unreal.Vector(1.2, 1.2, 1.8), materials["display"]),
        ("DemoSphere", -60.0, 0.0, 220.0, unreal.Vector(0.9, 0.9, 0.9), materials["glass"]),
        ("SideCanisterA", -140.0, -250.0, 90.0, unreal.Vector(0.55, 0.55, 1.6), materials["display"]),
        ("SideCanisterB", -140.0, 250.0, 90.0, unreal.Vector(0.55, 0.55, 1.6), materials["display"]),
    ]

    for piece_name, local_x, local_y, local_z, scale, piece_material in piece_defs:
        mesh = mesh_cube
        if "Sphere" in piece_name:
            mesh = mesh_sphere
        elif "Canister" in piece_name or piece_name == "Podium":
            mesh = mesh_cylinder
        spawn_static_mesh(
            f"Booth_{name}_{piece_name}",
            mesh,
            local_to_world(origin, yaw, local_x, local_y, local_z),
            unreal.Rotator(0.0, yaw, 0.0),
            scale,
            piece_material,
        )

    spawn_text(
        f"Vendor_{name}_Sign",
        name.upper(),
        local_to_world(origin, yaw, -225.0, 0.0, 470.0),
        unreal.Rotator(0.0, yaw + 180.0, 0.0),
        unreal.Vector(1.6, 1.6, 1.6),
        accent_color,
    )


def configure_bot(bot_actor, spec, visual_assets):
    local_character = bot_actor.get_component_by_class(unreal.LocalCharacterComponent)
    if not local_character:
        raise RuntimeError(f"{bot_actor.get_actor_label()} missing LocalCharacterComponent")

    set_prop(local_character, spec["name"], "speaker_name")
    set_prop(local_character, spec["desc"], "desc")
    set_prop(local_character, spec["directions"], "directions")
    set_prop(local_character, unreal.Name(spec["voice"]), "voice_id")
    set_prop(local_character, spec["conversation_radius"], "conversation_radius")
    set_prop(local_character, spec["conversation_radius"], "voice_attenuation_radius")
    try_set_prop(local_character, True, "use_local_sound", "b_use_local_sound")
    try_set_prop(local_character, True, "speak_streaming", "b_speak_streaming")
    try_set_prop(local_character, False, "use_project_settings_config", "b_use_project_settings_config")

    try:
        config = local_character.get_editor_property("character_config_override")
        config.temperature = spec["temperature"]
        config.max_tokens = spec["max_tokens"]
        config.top_p = spec["top_p"]
        config.presence_penalty = spec["presence_penalty"]
        config.frequency_penalty = spec["frequency_penalty"]
        local_character.set_editor_property("character_config_override", config)
    except Exception as exc:
        log(f"character_config_override_warning actor={bot_actor.get_actor_label()} warning={exc}")

    mesh_component = bot_actor.get_component_by_class(unreal.SkeletalMeshComponent)
    if mesh_component:
        if spec["body"] == "quinn":
            mesh_component.set_editor_property("skeletal_mesh_asset", visual_assets["quinn_mesh"])
            selected_material = visual_assets["quinn_b"] if spec["outfit"] else visual_assets["quinn_a"]
        else:
            mesh_component.set_editor_property("skeletal_mesh_asset", visual_assets["manny_mesh"])
            selected_material = visual_assets["manny_b"] if spec["outfit"] else visual_assets["manny_a"]

        mesh_component.set_editor_property("relative_location", unreal.Vector(0.0, 0.0, -96.0))
        mesh_component.set_editor_property("relative_rotation", unreal.Rotator(0.0, 0.0, 0.0))
        material_count = max(1, mesh_component.get_num_materials())
        for index in range(material_count):
            mesh_component.set_material(index, selected_material)

    tags = [
        unreal.Name("ConventionBot"),
        unreal.Name(f"CB_Body={'Quinn' if spec['body'] == 'quinn' else 'Manny'}"),
        unreal.Name(f"CB_Outfit={spec['outfit']}"),
        unreal.Name(f"CB_Roam={1 if spec['roam'] else 0}"),
        unreal.Name(f"CB_RoamSpeed={spec['roam_speed']}"),
        unreal.Name(f"CB_TalkRate={spec['talk_rate']}"),
        unreal.Name(f"CB_WalkRate={spec['walk_rate']}"),
        unreal.Name(f"CB_Voice={spec['voice']}"),
        unreal.Name(f"CB_FacePlayerRadius={spec['face_player_radius']}"),
        unreal.Name(f"CB_FaceBotRadius={spec['face_bot_radius']}"),
    ]
    if spec["patrol_offsets"]:
        patrol_text = ";".join(f"{p.x},{p.y},{p.z}" for p in spec["patrol_offsets"])
        tags.append(unreal.Name(f"CB_Patrol={patrol_text}"))
    bot_actor.set_editor_property("tags", tags)


def spawn_bot(spec, talker_asset, visual_assets):
    actor = unreal.EditorLevelLibrary.spawn_actor_from_object(
        talker_asset,
        spec["location"],
        unreal.Rotator(0.0, spec["yaw"], 0.0),
    )
    actor.set_actor_label(spec["label"])
    actor.set_folder_path("Convention/Bots")
    configure_bot(actor, spec, visual_assets)
    align_actor_to_floor(actor)
    return actor


def dedupe_spawned_bots(bot_specs):
    actor_subsystem = get_actor_subsystem()
    desired_locations = {spec["label"]: spec["location"] for spec in bot_specs}
    actors_by_label = {}

    for actor in get_all_actors():
        label = actor.get_actor_label()
        if label not in desired_locations:
            continue
        if "ConventionBot" not in [str(tag) for tag in actor.tags]:
            continue
        actors_by_label.setdefault(label, []).append(actor)

    deleted = 0
    for label, actors in actors_by_label.items():
        if len(actors) <= 1:
            continue

        target = desired_locations[label]
        actors.sort(key=lambda actor: (actor.get_actor_location() - target).size_squared())
        for stale_actor in actors[1:]:
            actor_subsystem.destroy_actor(stale_actor)
            deleted += 1

    if deleted:
        log(f"deduped_bots={deleted}")


def build_bot_specs():
    def booth_point(origin, yaw, x, y, z=120.0):
        return local_to_world(origin, yaw, x, y, z)

    booths = {
        "Quantum Circuits": {"origin": unreal.Vector(-650.0, -1550.0, 0.0), "yaw": 90.0},
        "Orbital Gardens": {"origin": unreal.Vector(1450.0, -1550.0, 0.0), "yaw": 90.0},
        "CryoSkin Labs": {"origin": unreal.Vector(-650.0, 1550.0, 0.0), "yaw": -90.0},
        "Telescope Forge": {"origin": unreal.Vector(1450.0, 1550.0, 0.0), "yaw": -90.0},
        "Cosmic Materials": {"origin": unreal.Vector(3250.0, 0.0, 0.0), "yaw": 180.0},
    }

    return [
        {
            "label": "Convention_DrVegaFinch",
            "name": "Dr. Vega Finch",
            "body": "manny",
            "outfit": 0,
            "location": booth_point(booths["Quantum Circuits"]["origin"], booths["Quantum Circuits"]["yaw"], -80.0, -170.0),
            "yaw": booths["Quantum Circuits"]["yaw"] - 90.0,
            "roam": False,
            "patrol_offsets": [],
            "roam_speed": 145.0,
            "walk_rate": 1.0,
            "talk_rate": 0.35,
            "face_player_radius": 800.0,
            "face_bot_radius": 650.0,
            "voice": "Ryan",
            "conversation_radius": 700.0,
            "temperature": 0.30,
            "top_p": 0.82,
            "presence_penalty": 0.15,
            "frequency_penalty": 0.28,
            "max_tokens": 44,
            "desc": "A superconducting chip architect showing off radiation-hardened logic boards for deep space robots.",
            "directions": "Pitch compact quantum control chips. Mention low heat, stable timing, and booth demos. Keep replies crisp and polished.",
        },
        {
            "label": "Convention_IrisQuark",
            "name": "Iris Quark",
            "body": "quinn",
            "outfit": 1,
            "location": booth_point(booths["Quantum Circuits"]["origin"], booths["Quantum Circuits"]["yaw"], 760.0, 180.0),
            "yaw": 0.0,
            "roam": True,
            "patrol_offsets": [unreal.Vector(-140.0, -240.0, 0.0), unreal.Vector(120.0, 240.0, 0.0)],
            "roam_speed": 132.0,
            "walk_rate": 1.05,
            "talk_rate": 0.55,
            "face_player_radius": 950.0,
            "face_bot_radius": 700.0,
            "voice": "Serena",
            "conversation_radius": 700.0,
            "temperature": 0.44,
            "top_p": 0.90,
            "presence_penalty": 0.25,
            "frequency_penalty": 0.35,
            "max_tokens": 52,
            "desc": "A demo runner who loves inviting visitors to touch every glowing circuit in the booth.",
            "directions": "Sound energetic and welcoming. Encourage hands-on demos and quick comparisons between old silicon and new quantum boards.",
        },
        {
            "label": "Convention_SeraBloom",
            "name": "Sera Bloom",
            "body": "quinn",
            "outfit": 0,
            "location": booth_point(booths["Orbital Gardens"]["origin"], booths["Orbital Gardens"]["yaw"], -80.0, -170.0),
            "yaw": booths["Orbital Gardens"]["yaw"] - 90.0,
            "roam": False,
            "patrol_offsets": [],
            "roam_speed": 140.0,
            "walk_rate": 1.0,
            "talk_rate": 0.5,
            "face_player_radius": 820.0,
            "face_bot_radius": 650.0,
            "voice": "Vivian",
            "conversation_radius": 700.0,
            "temperature": 0.33,
            "top_p": 0.86,
            "presence_penalty": 0.18,
            "frequency_penalty": 0.24,
            "max_tokens": 46,
            "desc": "A calm orbital botanist selling seed systems for greenhouse rings and lunar domes.",
            "directions": "Talk about resilient crops, microgravity roots, and beautiful living habitats. Keep the tone thoughtful and confident.",
        },
        {
            "label": "Convention_DexPollen",
            "name": "Dex Pollen",
            "body": "manny",
            "outfit": 1,
            "location": booth_point(booths["Orbital Gardens"]["origin"], booths["Orbital Gardens"]["yaw"], 760.0, -180.0),
            "yaw": 0.0,
            "roam": True,
            "patrol_offsets": [unreal.Vector(-120.0, -260.0, 0.0), unreal.Vector(160.0, 260.0, 0.0)],
            "roam_speed": 136.0,
            "walk_rate": 1.1,
            "talk_rate": 0.38,
            "face_player_radius": 960.0,
            "face_bot_radius": 700.0,
            "voice": "Eric",
            "conversation_radius": 700.0,
            "temperature": 0.52,
            "top_p": 0.92,
            "presence_penalty": 0.28,
            "frequency_penalty": 0.32,
            "max_tokens": 54,
            "desc": "A field engineer demonstrating pollination drones and nutrient mist rings.",
            "directions": "Be upbeat and practical. Explain how tiny helper bots keep orbital farms alive during long missions.",
        },
        {
            "label": "Convention_MariusFrost",
            "name": "Marius Frost",
            "body": "manny",
            "outfit": 1,
            "location": booth_point(booths["CryoSkin Labs"]["origin"], booths["CryoSkin Labs"]["yaw"], -80.0, 170.0),
            "yaw": booths["CryoSkin Labs"]["yaw"] + 90.0,
            "roam": False,
            "patrol_offsets": [],
            "roam_speed": 135.0,
            "walk_rate": 0.95,
            "talk_rate": 0.35,
            "face_player_radius": 820.0,
            "face_bot_radius": 650.0,
            "voice": "Brutus",
            "conversation_radius": 700.0,
            "temperature": 0.26,
            "top_p": 0.80,
            "presence_penalty": 0.12,
            "frequency_penalty": 0.20,
            "max_tokens": 42,
            "desc": "A severe materials specialist selling cryo-adaptive suits and thermal skins.",
            "directions": "Speak with authority about extreme cold, insulation layers, and survival on dark moons. Keep it concise and sharp.",
        },
        {
            "label": "Convention_KiraLens",
            "name": "Kira Lens",
            "body": "quinn",
            "outfit": 1,
            "location": booth_point(booths["CryoSkin Labs"]["origin"], booths["CryoSkin Labs"]["yaw"], 760.0, 180.0),
            "yaw": 180.0,
            "roam": True,
            "patrol_offsets": [unreal.Vector(-120.0, -250.0, 0.0), unreal.Vector(140.0, 250.0, 0.0)],
            "roam_speed": 132.0,
            "walk_rate": 1.0,
            "talk_rate": 0.55,
            "face_player_radius": 940.0,
            "face_bot_radius": 700.0,
            "voice": "OnoAnna",
            "conversation_radius": 700.0,
            "temperature": 0.40,
            "top_p": 0.88,
            "presence_penalty": 0.20,
            "frequency_penalty": 0.30,
            "max_tokens": 50,
            "desc": "A product scout comparing thermal optics, visor coatings, and frost-safe field gear.",
            "directions": "Invite visitors to inspect the optics and brag about clarity in blizzards, vacuum dust, and cryo fog.",
        },
        {
            "label": "Convention_TansyLoop",
            "name": "Prof. Tansy Loop",
            "body": "quinn",
            "outfit": 0,
            "location": booth_point(booths["Telescope Forge"]["origin"], booths["Telescope Forge"]["yaw"], -80.0, 170.0),
            "yaw": booths["Telescope Forge"]["yaw"] + 90.0,
            "roam": False,
            "patrol_offsets": [],
            "roam_speed": 140.0,
            "walk_rate": 1.0,
            "talk_rate": 0.5,
            "face_player_radius": 840.0,
            "face_bot_radius": 650.0,
            "voice": "Sohee",
            "conversation_radius": 700.0,
            "temperature": 0.34,
            "top_p": 0.84,
            "presence_penalty": 0.14,
            "frequency_penalty": 0.22,
            "max_tokens": 45,
            "desc": "An observatory designer presenting portable telescopes, gravitic mounts, and survey optics.",
            "directions": "Sound brilliant but approachable. Mention crisp imaging, fold-out mirrors, and star-mapping on remote worlds.",
        },
        {
            "label": "Convention_PipResonance",
            "name": "Pip Resonance",
            "body": "manny",
            "outfit": 0,
            "location": booth_point(booths["Telescope Forge"]["origin"], booths["Telescope Forge"]["yaw"], 760.0, -180.0),
            "yaw": 180.0,
            "roam": True,
            "patrol_offsets": [unreal.Vector(-150.0, -250.0, 0.0), unreal.Vector(150.0, 250.0, 0.0)],
            "roam_speed": 138.0,
            "walk_rate": 1.12,
            "talk_rate": 0.40,
            "face_player_radius": 960.0,
            "face_bot_radius": 700.0,
            "voice": "Dylan",
            "conversation_radius": 700.0,
            "temperature": 0.58,
            "top_p": 0.93,
            "presence_penalty": 0.30,
            "frequency_penalty": 0.36,
            "max_tokens": 55,
            "desc": "A jittery booth host who keeps sending visitors toward the live lens calibration rig.",
            "directions": "Be fast, excited, and very trade-show friendly. Offer live demos and call out exact use cases for explorers and survey teams.",
        },
        {
            "label": "Convention_GideonVale",
            "name": "Gideon Vale",
            "body": "manny",
            "outfit": 0,
            "location": booth_point(booths["Cosmic Materials"]["origin"], booths["Cosmic Materials"]["yaw"], -80.0, -170.0),
            "yaw": booths["Cosmic Materials"]["yaw"] - 180.0,
            "roam": False,
            "patrol_offsets": [],
            "roam_speed": 140.0,
            "walk_rate": 1.0,
            "talk_rate": 0.36,
            "face_player_radius": 840.0,
            "face_bot_radius": 650.0,
            "voice": "UncleFu",
            "conversation_radius": 750.0,
            "temperature": 0.22,
            "top_p": 0.78,
            "presence_penalty": 0.10,
            "frequency_penalty": 0.18,
            "max_tokens": 40,
            "desc": "A veteran vendor who sells metamaterials, lab-safe housings, and radiation baffles.",
            "directions": "Speak like an experienced supplier who has seen every failure mode. Focus on reliability and safety with short, grounded replies.",
        },
        {
            "label": "Convention_NovaCalder",
            "name": "Nova Calder",
            "body": "quinn",
            "outfit": 1,
            "location": booth_point(booths["Cosmic Materials"]["origin"], booths["Cosmic Materials"]["yaw"], 780.0, 0.0),
            "yaw": 270.0,
            "roam": True,
            "patrol_offsets": [unreal.Vector(-220.0, -220.0, 0.0), unreal.Vector(220.0, 220.0, 0.0)],
            "roam_speed": 134.0,
            "walk_rate": 1.08,
            "talk_rate": 0.56,
            "face_player_radius": 980.0,
            "face_bot_radius": 760.0,
            "voice": "Aiden",
            "conversation_radius": 750.0,
            "temperature": 0.47,
            "top_p": 0.89,
            "presence_penalty": 0.27,
            "frequency_penalty": 0.31,
            "max_tokens": 53,
            "desc": "A rover-fabrication specialist guiding people through smart alloys and self-healing shell panels.",
            "directions": "Talk like a charismatic prototype lead. Mention field tests, impact resilience, and how fast the materials can be reconfigured.",
        },
    ], booths


def populate_level():
    configure_world()
    cleanup_level()
    log("assets_loading")

    assets = {
        "cube": load_asset("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube"),
        "sphere": load_asset("/Game/StarterContent/Shapes/Shape_Sphere.Shape_Sphere"),
        "cylinder": load_asset("/Game/LevelPrototyping/Meshes/SM_Cylinder.SM_Cylinder"),
        "table": load_asset("/Game/StarterContent/Props/SM_TableRound.SM_TableRound"),
        "chair": load_asset("/Game/StarterContent/Props/SM_Chair.SM_Chair"),
        "talker": load_asset(TALKER_ASSET_PATH),
    }
    materials = {
        "floor": load_asset("/Game/StarterContent/Materials/M_Concrete_Panels.M_Concrete_Panels"),
        "wall": load_asset("/Game/StarterContent/Materials/M_Tech_Panel.M_Tech_Panel"),
        "accent": load_asset("/Game/StarterContent/Materials/M_Tech_Hex_Tile_Pulse.M_Tech_Hex_Tile_Pulse"),
        "display": load_asset("/Game/StarterContent/Materials/M_Metal_Chrome.M_Metal_Chrome"),
        "glass": load_asset("/Game/StarterContent/Materials/M_Glass.M_Glass"),
        "metal": load_asset("/Game/StarterContent/Materials/M_Metal_Brushed_Nickel.M_Metal_Brushed_Nickel"),
    }
    visual_assets = {
        "manny_mesh": load_asset("/Game/Characters/Mannequins/Meshes/SKM_Manny.SKM_Manny"),
        "quinn_mesh": load_asset("/Game/Characters/Mannequins/Meshes/SKM_Quinn.SKM_Quinn"),
        "manny_a": load_asset("/Game/Characters/Mannequins/Materials/Instances/Manny/MI_Manny_01.MI_Manny_01"),
        "manny_b": load_asset("/Game/Characters/Mannequins/Materials/Instances/Manny/MI_Manny_02.MI_Manny_02"),
        "quinn_a": load_asset("/Game/Characters/Mannequins/Materials/Instances/Quinn/MI_Quinn_01.MI_Quinn_01"),
        "quinn_b": load_asset("/Game/Characters/Mannequins/Materials/Instances/Quinn/MI_Quinn_02.MI_Quinn_02"),
    }

    create_main_stage(assets["cube"], assets["sphere"], assets["cylinder"], materials)
    log("main_stage_complete")
    create_lounge(assets["table"], assets["chair"], assets["sphere"], materials)
    log("lounge_complete")

    booth_materials = [
        load_asset("/Game/StarterContent/Materials/M_Metal_Copper.M_Metal_Copper"),
        load_asset("/Game/StarterContent/Materials/M_Tech_Hex_Tile.M_Tech_Hex_Tile"),
        load_asset("/Game/StarterContent/Materials/M_Metal_Gold.M_Metal_Gold"),
        load_asset("/Game/StarterContent/Materials/M_Concrete_Panels.M_Concrete_Panels"),
        load_asset("/Game/StarterContent/Materials/M_Metal_Steel.M_Metal_Steel"),
    ]
    booth_colors = [
        unreal.Color(90, 255, 250, 255),
        unreal.Color(120, 255, 140, 255),
        unreal.Color(255, 230, 110, 255),
        unreal.Color(160, 220, 255, 255),
        unreal.Color(255, 180, 120, 255),
    ]

    bot_specs, booths = build_bot_specs()
    for (booth_name, booth_def), booth_material, booth_color in zip(booths.items(), booth_materials, booth_colors):
        log(f"creating_booth={booth_name}")
        create_booth(booth_name, booth_def["origin"], booth_def["yaw"], booth_material, booth_color, materials, assets)
    log("booths_complete")

    for spec in bot_specs:
        log(f"spawning_bot={spec['label']}")
        spawn_bot(spec, assets["talker"], visual_assets)
    log("bots_complete")
    dedupe_spawned_bots(bot_specs)

    unreal.EditorLevelLibrary.save_current_level()
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    log(f"spawned_bots={len(bot_specs)}")


if __name__ == "__main__":
    populate_level()
