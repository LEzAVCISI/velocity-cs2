#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/logging/logging.hpp>
#include <protection/game_addresses.hpp>
#include "../addresses.hpp"

namespace addresses::globals {

	bool initialize () {
		source2client              = INTERFACE_ ("Source2Client002");
		if ( !source2client && addresses::modules::client )
			source2client = addresses::modules::client + 0x23A48D0;

		panorama                   = INTERFACE_ ("PanoramaUIEngine001");
		if ( !panorama && addresses::modules::panorama )
			panorama = addresses::modules::panorama + 0x50FD60;

		source2engine_to_client    = INTERFACE_ ("Source2EngineToClient001");
		if ( !source2engine_to_client && addresses::modules::engine2 )
			source2engine_to_client = addresses::modules::engine2 + 0x6125A0;

		scene_system               = INTERFACE_ ("SceneSystem_002");
		if ( !scene_system && addresses::modules::scene_system )
			scene_system = addresses::modules::scene_system + 0x911530;

		material_system            = INTERFACE_ ("VMaterialSystem2_001");
		if ( !material_system && addresses::modules::material_system2 )
			material_system = addresses::modules::material_system2 + 0x15C800;

		schema_system              = INTERFACE_ ("SchemaSystem_001");
		if ( !schema_system && addresses::modules::schema_system )
			schema_system = addresses::modules::schema_system + 0x75730;

		input_system               = INTERFACE_ ("InputSystemVersion001");
		if ( !input_system && addresses::modules::input_system )
			input_system = addresses::modules::input_system + 0x45BA0;

		particle_system_mgr        = INTERFACE_ ("ParticleSystemMgr003");
		if ( !particle_system_mgr && addresses::modules::particles )
			particle_system_mgr = addresses::modules::particles + 0x5FEAC0;

		cvar                       = (interfaces::c_engine_cvar*)INTERFACE_ ("VEngineCvar007");
		if ( !cvar && addresses::modules::tier0 )
			cvar = reinterpret_cast<interfaces::c_engine_cvar*>( addresses::modules::tier0 + 0x3A44F0 );

		source2client_prediction   = INTERFACE_ ("Source2ClientPrediction001");
		if ( !source2client_prediction && addresses::modules::client )
			source2client_prediction = addresses::modules::client + 0x23AA020;

		network_client_service     = INTERFACE_ ("NetworkClientService_001");
		if ( !network_client_service && addresses::modules::engine2 )
			network_client_service = addresses::modules::engine2 + 0x90D410;

		resource_system            = INTERFACE_ ("ResourceSystem013");
		if ( !resource_system && addresses::modules::resource_system )
			resource_system = addresses::modules::resource_system + 0x81680;

		localize                   = INTERFACE_ ("Localize_001");
		if ( !localize && addresses::modules::localize )
			localize = addresses::modules::localize + 0x58100;

		mesh_system                = INTERFACE_ ("MeshSystem001");
		if ( !mesh_system && addresses::modules::mesh_system )
			mesh_system = addresses::modules::mesh_system + 0x16BDE0;

		file_system                = INTERFACE_ ("VFileSystem017");
		if ( !file_system && addresses::modules::file_system_stdio )
			file_system = addresses::modules::file_system_stdio + 0x2133C0;

		csgo_input             = PATTERN (patterns::csgo_input);
		if ( !csgo_input && addresses::modules::client )
			csgo_input = addresses::modules::client + offsets::client_dll::dwCSGOInput;

		entity_list            = PATTERN (patterns::entity_list);
		if ( !entity_list && addresses::modules::client )
			entity_list = addresses::modules::client + offsets::client_dll::dwEntityList;

		local_player_controller = PATTERN (patterns::local_player_controller);
		if ( !local_player_controller && addresses::modules::client )
			local_player_controller = addresses::modules::client + offsets::client_dll::dwLocalPlayerController;

		global_vars            = PATTERN (patterns::global_vars);
		if ( !global_vars && addresses::modules::client )
			global_vars = addresses::modules::client + offsets::client_dll::dwGlobalVars;

		view_matrix            = PATTERN (patterns::view_matrix);
		if ( !view_matrix && addresses::modules::client )
			view_matrix = addresses::modules::client + offsets::client_dll::dwViewMatrix;

		game_rules             = PATTERN (patterns::game_rules);
		if ( !game_rules && addresses::modules::client )
			game_rules = addresses::modules::client + offsets::client_dll::dwGameRules;

		light_data_queue       = PATTERN (patterns::light_data_queue);
		particle_manager       = PATTERN (patterns::particle_manager);
		game_event_manager     = PATTERN (patterns::game_event_manager);
		game_trace_manager     = PATTERN (patterns::game_trace_manager);
		render_game_system_storage = PATTERN (patterns::render_game_system_storage);
		material_manager       = PATTERN (patterns::material_manager);

		game_entity_system     = PATTERN (patterns::game_entity_system);
		if ( !game_entity_system && addresses::modules::client )
			game_entity_system = addresses::modules::client + offsets::client_dll::dwGameEntitySystem;

		weapon_recoil_data     = PATTERN (patterns::weapon_recoil_data);
		hud                    = PATTERN (patterns::hud);
		prediction_seed        = PATTERN (patterns::prediction_seed);
		simulation_player      = PATTERN (patterns::simulation_player);
		prediction_player      = PATTERN (patterns::prediction_player);

		planted_c4             = PATTERN (patterns::planted_c4);
		if ( !planted_c4 && addresses::modules::client )
			planted_c4 = addresses::modules::client + offsets::client_dll::dwPlantedC4;

		item_system            = PATTERN (patterns::item_system);
		frame_input_ring_idx   = PATTERN (patterns::frame_input_ring_idx);
		frame_input_ring_base  = PATTERN (patterns::frame_input_ring_base);
		prediction_state       = PATTERN (patterns::prediction_state);

		if (const auto ptr = MODULE_EXPORT("tier0.dll:g_pMemAlloc"))
			mem_alloc = *reinterpret_cast<std::uintptr_t*>(ptr);

		const std::pair<std::string_view, std::uintptr_t> required_globals[] {
			{ "csgo_input", csgo_input },
			{ "entity_list", entity_list },
			{ "local_player_controller", local_player_controller },
			{ "global_vars", global_vars },
			{ "view_matrix", view_matrix },
			{ "game_rules", game_rules },
			{ "light_data_queue", light_data_queue },
			{ "particle_manager", particle_manager },
			{ "game_event_manager", game_event_manager },
			{ "game_trace_manager", game_trace_manager },
			{ "render_game_system_storage", render_game_system_storage },
			{ "mem_alloc", mem_alloc },
			{ "material_manager", material_manager },
			{ "game_entity_system", game_entity_system },
			{ "weapon_recoil_data", weapon_recoil_data },
			{ "hud", hud },
			{ "prediction_seed", prediction_seed },
			{ "simulation_player", simulation_player },
			{ "prediction_player", prediction_player },
			{ "planted_c4", planted_c4 },
			{ "item_system", item_system },
			{ "network_client_service", network_client_service },
			{ "frame_input_ring_idx", frame_input_ring_idx },
			{ "frame_input_ring_base", frame_input_ring_base },
			{ "prediction_state", prediction_state },
		};

		auto initialized = true;
		for (const auto& [name, address] : required_globals) {
			if (!address) {
				logging::console::print (xs ("[error] global address not initialized | {}"), name);
				initialized = false;
			}
		}

		return initialized;
	}

} // namespace addresses::globals
