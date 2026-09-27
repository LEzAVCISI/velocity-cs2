#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/systems/systems.hpp>
#include <core/features/features.hpp>
#include <core/settings.hpp>
#include <protection/game_addresses.hpp>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

namespace features::misc {

	namespace detail {

		// Skid model_changer: packs ship partial pieces (cloth meshes,
		// no-skeleton variants, physics/LOD fragments) alongside the full
		// character model. Those have no proper player skeleton, so SetModel
		// on the pawn crashes deep in bone/anim setup. Skip anything whose
		// name carries a fragment marker so only full models reach the list.
		[[nodiscard]] static bool is_fragment_model( std::string name )
		{
			std::transform( name.begin( ), name.end( ), name.begin( ), [ ]( unsigned char c )
				{
					return static_cast< char >( std::tolower( c ) );
				} );

			static constexpr const char* k_markers[ ]{
				"nohitbox", "no_hitbox", "nohbox", "hitbox",
				"cloth", "swim", "prefab", "arm",
				"_lod", "gib", "phys", "ragdoll",
				"attach", "_part", "helmet", "hat", "mask", "prop", "normal", "player_model", "dress",
				"sk2model", "gfl2", "sleeve",
			};

			for ( const auto* marker : k_markers )
			{
				if ( name.find( marker ) != std::string::npos )
				{
					return true;
				}
			}

			return false;
		}

		[[nodiscard]] static std::wstring models_directory( )
		{
			wchar_t app_data[ MAX_PATH ]{};
			if ( FAILED( SHGetFolderPathW( nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data ) ) )
			{
				return {};
			}

			const auto root = std::wstring( app_data ) + L"\\shawsoftware";
			const auto models = root + L"\\models";

			CreateDirectoryW( root.c_str( ), nullptr );
			CreateDirectoryW( models.c_str( ), nullptr );

			return models;
		}

		[[nodiscard]] static std::wstring legacy_models_directory( )
		{
			wchar_t app_data[ MAX_PATH ]{};
			if ( FAILED( SHGetFolderPathW( nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data ) ) )
			{
				return {};
			}

			return std::wstring( app_data ) + L"\\cot.gg\\models";
		}

		[[nodiscard]] static std::wstring weapons_directory( )
		{
			wchar_t app_data[ MAX_PATH ]{};
			if ( FAILED( SHGetFolderPathW( nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data ) ) )
			{
				return {};
			}

			const auto root = std::wstring( app_data ) + L"\\shawsoftware";
			const auto weapons = root + L"\\models\\weapons";

			CreateDirectoryW( root.c_str( ), nullptr );
			CreateDirectoryW( ( root + L"\\models" ).c_str( ), nullptr );
			CreateDirectoryW( weapons.c_str( ), nullptr );

			return weapons;
		}

		[[nodiscard]] static std::wstring legacy_weapons_directory( )
		{
			wchar_t app_data[ MAX_PATH ]{};
			if ( FAILED( SHGetFolderPathW( nullptr, CSIDL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, app_data ) ) )
			{
				return {};
			}

			return std::wstring( app_data ) + L"\\cot.gg\\models\\weapons";
		}

		// Get game root directory (e.g. "C:\...\Counter-Strike Global Offensive\game")
		[[nodiscard]] static std::string game_root_directory( )
		{
			char cwd_buf[ MAX_PATH ]{};
			if ( !GetCurrentDirectoryA( MAX_PATH, cwd_buf ) )
			{
				return {};
			}

			std::string path = cwd_buf;
			auto pos = path.find( "bin\\win64" );
			if ( pos != std::string::npos )
			{
				return path.substr( 0, pos );
			}

			return path + "\\";
		}

		// Deploy player .vmdl_c files from user folder into game/csgo/characters/models/shawsoftware/
		// Returns engine-relative path like "characters/models/shawsoftware/filename.vmdl"
		[[nodiscard]] static std::string deploy_player_model( const fs::path& source_file )
		{
			const auto game_root = game_root_directory( );
			if ( game_root.empty( ) )
			{
				return {};
			}

			const auto addon_models = game_root + "csgo\\characters\\models\\shawsoftware";
			fs::create_directories( addon_models );

			const auto filename = source_file.filename( ).string( );
			const auto dest_path = addon_models + "\\" + filename;

			// Copy if not exists or source is newer
			std::error_code ec;
			if ( !fs::exists( dest_path ) || fs::last_write_time( source_file, ec ) > fs::last_write_time( dest_path, ec ) )
			{
				fs::copy_file( source_file, dest_path, fs::copy_options::overwrite_existing, ec );
			}

			// Build engine-relative path (strip _c extension for engine path)
			std::string engine_path = "characters/models/shawsoftware/" + filename;
			if ( engine_path.ends_with( "_c" ) )
			{
				engine_path = engine_path.substr( 0, engine_path.length( ) - 2 );
			}

			return engine_path;
		}

		// Deploy weapon .vmdl_c files from user folder into game/csgo/models/weapons/shawsoftware/
		// Returns engine-relative path like "models/weapons/shawsoftware/filename.vmdl"
		[[nodiscard]] static std::string deploy_weapon_model( const fs::path& source_file )
		{
			const auto game_root = game_root_directory( );
			if ( game_root.empty( ) )
			{
				return {};
			}

			const auto addon_models = game_root + "csgo\\models\\weapons\\shawsoftware";
			fs::create_directories( addon_models );

			const auto filename = source_file.filename( ).string( );
			const auto dest_path = addon_models + "\\" + filename;

			// Copy if not exists or source is newer
			std::error_code ec;
			if ( !fs::exists( dest_path ) || fs::last_write_time( source_file, ec ) > fs::last_write_time( dest_path, ec ) )
			{
				fs::copy_file( source_file, dest_path, fs::copy_options::overwrite_existing, ec );
			}

			// Build engine-relative path (strip _c extension for engine path)
			std::string engine_path = "models/weapons/shawsoftware/" + filename;
			if ( engine_path.ends_with( "_c" ) )
			{
				engine_path = engine_path.substr( 0, engine_path.length( ) - 2 );
			}

			return engine_path;
		}

	} // namespace detail

	int custom_models::selected_index( ) const
	{
		return settings::g_misc.m_custom_models.selected_index.value;
	}

	void custom_models::set_selected_index( int index )
	{
		settings::g_misc.m_custom_models.selected_index.value = index;
	}

	int custom_models::selected_gun_index( ) const
	{
		return settings::g_misc.m_custom_models.selected_gun_index.value;
	}

	void custom_models::set_selected_gun_index( int index )
	{
		settings::g_misc.m_custom_models.selected_gun_index.value = index;
	}

	void custom_models::open_folder( ) const
	{
		const auto dir = detail::models_directory( );
		if ( !dir.empty( ) )
		{
			ShellExecuteW( nullptr, L"open", dir.c_str( ), nullptr, nullptr, SW_SHOWNORMAL );
		}
	}

	void custom_models::open_weapons_folder( ) const
	{
		const auto dir = detail::weapons_directory( );
		if ( !dir.empty( ) )
		{
			ShellExecuteW( nullptr, L"open", dir.c_str( ), nullptr, nullptr, SW_SHOWNORMAL );
		}
	}

	void custom_models::scan_models( )
	{
		this->m_models.clear( );
		this->m_models.push_back( { "[ default / off ]", "" } );

		this->m_gun_models.clear( );
		this->m_gun_models.push_back( { "[ default / off ]", "" } );

		// 1. Official Built-in CS2 Agent Models
		static const std::vector<std::pair<std::string, std::string>> builtin_agents = {
			{ "FBI Special Agent Ava", "characters/models/ctm_fbi/ctm_fbi_variantb.vmdl" },
			{ "Operator | FBI SWAT", "characters/models/ctm_fbi/ctm_fbi_variantf.vmdl" },
			{ "Markus Delrow | FBI HRT", "characters/models/ctm_fbi/ctm_fbi_variantg.vmdl" },
			{ "Michael Syfers | FBI Sniper", "characters/models/ctm_fbi/ctm_fbi_varianth.vmdl" },
			{ "Cmdr. Frank 'Wet Sox' Baroud | Frogman", "characters/models/ctm_diver/ctm_diver_varianta.vmdl" },
			{ "Lieutenant Rex Krikey | Frogman", "characters/models/ctm_diver/ctm_diver_variantb.vmdl" },
			{ "Buckshot | NSWC SEAL", "characters/models/ctm_st6/ctm_st6_variante.vmdl" },
			{ "'Two Times' McCoy | USAF TACP", "characters/models/ctm_st6/ctm_st6_variantm.vmdl" },
			{ "Officer Jacques Beltram | GIGN", "characters/models/ctm_gign/ctm_gign.vmdl" },
			{ "D Squadron Officer | NZSAS", "characters/models/ctm_sas/ctm_sas_variantf.vmdl" },
			{ "SAS Default", "characters/models/ctm_sas/ctm_sas.vmdl" },
			{ "SWAT Default", "characters/models/ctm_swat/ctm_swat.vmdl" },
			{ "Cmdr. Mae 'Dead Cold' Jamison | SWAT", "characters/models/ctm_swat/ctm_swat_variantg.vmdl" },
			{ "1st Lieutenant Farlow | SWAT", "characters/models/ctm_swat/ctm_swat_varianth.vmdl" },
			{ "Sir Bloody Miami Darryl | The Professionals", "characters/models/tm_professional/tm_professional_varf.vmdl" },
			{ "Sir Bloody Silent Darryl | The Professionals", "characters/models/tm_professional/tm_professional_varf1.vmdl" },
			{ "Sir Bloody Skullhead Darryl | The Professionals", "characters/models/tm_professional/tm_professional_varf2.vmdl" },
			{ "Sir Bloody Darryl Royale | The Professionals", "characters/models/tm_professional/tm_professional_varf3.vmdl" },
			{ "Sir Bloody Loudmouth Darryl | The Professionals", "characters/models/tm_professional/tm_professional_varf4.vmdl" },
			{ "Safecracker Voltzmann | The Professionals", "characters/models/tm_professional/tm_professional_varg.vmdl" },
			{ "Little Kev | The Professionals", "characters/models/tm_professional/tm_professional_varh.vmdl" },
			{ "Number K | The Professionals", "characters/models/tm_professional/tm_professional_vari.vmdl" },
			{ "Getaway Sally | The Professionals", "characters/models/tm_professional/tm_professional_varj.vmdl" },
			{ "Crasswater The Forgotten | Guerrilla", "characters/models/tm_jungle/tm_jungle_varianta.vmdl" },
			{ "'Medium Rare' Crasswater | Guerrilla", "characters/models/tm_jungle/tm_jungle_variantb.vmdl" },
			{ "Vypa Sista of the Revolution | Guerrilla", "characters/models/tm_jungle/tm_jungle_variantc.vmdl" },
			{ "Col. Mangos Dabisi | Guerrilla", "characters/models/tm_jungle/tm_jungle_variantd.vmdl" },
			{ "Trapper | Guerrilla", "characters/models/tm_jungle/tm_jungle_variante.vmdl" },
			{ "The Elite Mr. Muhlik | Elite Crew", "characters/models/tm_leet/tm_leet_variantf.vmdl" },
			{ "Prof. Shahmat | Elite Crew", "characters/models/tm_leet/tm_leet_varianti.vmdl" },
			{ "Osiris | Elite Crew", "characters/models/tm_leet/tm_leet_varianth.vmdl" },
			{ "Ground Rebel | Elite Crew", "characters/models/tm_leet/tm_leet_variantg.vmdl" },
			{ "Maximus | Sabre", "characters/models/tm_balkan/tm_balkan_varianti.vmdl" },
			{ "Dragomir | Sabre", "characters/models/tm_balkan/tm_balkan_variantf.vmdl" },
			{ "Rezan The Ready | Sabre", "characters/models/tm_balkan/tm_balkan_variantg.vmdl" },
			{ "Blackwolf | Sabre", "characters/models/tm_balkan/tm_balkan_variantj.vmdl" },
			{ "Phoenix Default", "characters/models/tm_phoenix/tm_phoenix.vmdl" },
			{ "Slingshot | Phoenix", "characters/models/tm_phoenix/tm_phoenix_variantg.vmdl" },
			{ "Enforcer | Phoenix", "characters/models/tm_phoenix/tm_phoenix_variantf.vmdl" },
			{ "Soldier | Phoenix", "characters/models/tm_phoenix/tm_phoenix_varianth.vmdl" }
		};

		for ( const auto& [agent_name, agent_path] : builtin_agents )
		{
			this->m_models.push_back( { agent_name, agent_path } );
		}

		// 2. Scan Custom Player Models (%APPDATA%\shawsoftware\models)
		auto scan_players = [this]( const std::wstring& dir_path )
		{
			if ( dir_path.empty( ) || !fs::exists( dir_path ) )
			{
				return;
			}

			std::error_code ec;
			for ( const auto& entry : fs::recursive_directory_iterator( dir_path, fs::directory_options::skip_permission_denied, ec ) )
			{
				if ( ec ) break;
				if ( !entry.is_regular_file( ) ) continue;

				// Skip weapon subfolder for player models
				if ( entry.path( ).string( ).find( "weapons" ) != std::string::npos )
				{
					continue;
				}

				const auto ext = entry.path( ).extension( ).string( );
				if ( ext != ".vmdl_c" && ext != ".vmdl" )
				{
					continue;
				}

				// Skid: drop fragment/no-skeleton pieces and digit-leading
				// names before deploying — they crash SetModel on apply.
				const auto stem = entry.path( ).stem( ).string( );
				if ( stem.empty( ) || detail::is_fragment_model( stem ) || std::isdigit( static_cast< unsigned char >( stem[ 0 ] ) ) )
				{
					continue;
				}

				// Deploy into game characters folder and get engine-relative path
				const auto engine_path = detail::deploy_player_model( entry.path( ) );
				if ( engine_path.empty( ) )
				{
					continue;
				}

				std::string display_name = "[custom] " + entry.path( ).stem( ).string( );
				if ( display_name.ends_with( ".vmdl" ) )
				{
					display_name = display_name.substr( 0, display_name.length( ) - 5 );
				}

				// Avoid duplicates
				bool duplicate = false;
				for ( const auto& m : this->m_models )
				{
					if ( m.path == engine_path )
					{
						duplicate = true;
						break;
					}
				}

				if ( !duplicate )
				{
					this->m_models.push_back( { display_name, engine_path } );
				}
			}
		};

		scan_players( detail::models_directory( ) );
		scan_players( detail::legacy_models_directory( ) );

		// 2. Scan Weapon Models (%APPDATA%\shawsoftware\models\weapons)
		//    Deploy .vmdl_c files into game/csgo_addons/shawsoftware/models/ so engine can resolve them
		auto scan_weapons = [this]( const std::wstring& dir_path )
		{
			if ( dir_path.empty( ) || !fs::exists( dir_path ) )
			{
				return;
			}

			std::error_code ec;
			for ( const auto& entry : fs::recursive_directory_iterator( dir_path, fs::directory_options::skip_permission_denied, ec ) )
			{
				if ( ec ) break;
				if ( !entry.is_regular_file( ) ) continue;

				const auto ext = entry.path( ).extension( ).string( );
				if ( ext != ".vmdl_c" && ext != ".vmdl" )
				{
					continue;
				}

				// Deploy into game addons folder and get engine-relative path
				const auto engine_path = detail::deploy_weapon_model( entry.path( ) );
				if ( engine_path.empty( ) )
				{
					continue;
				}

				std::string display_name = "[weapon] " + entry.path( ).stem( ).string( );
				if ( display_name.ends_with( ".vmdl" ) )
				{
					display_name = display_name.substr( 0, display_name.length( ) - 5 );
				}

				// Avoid duplicates
				bool duplicate = false;
				for ( const auto& m : this->m_gun_models )
				{
					if ( m.path == engine_path )
					{
						duplicate = true;
						break;
					}
				}

				if ( !duplicate )
				{
					this->m_gun_models.push_back( { display_name, engine_path } );
				}
			}
		};

		scan_weapons( detail::weapons_directory( ) );
		scan_weapons( detail::legacy_weapons_directory( ) );

		// 3. Scan csgo/characters/models if exists in game directory
		char cwd_buf[ MAX_PATH ]{};
		if ( GetCurrentDirectoryA( MAX_PATH, cwd_buf ) )
		{
			std::string game_root = cwd_buf;
			auto pos = game_root.find( "bin\\win64" );
			if ( pos != std::string::npos )
			{
				game_root.replace( pos, 9, "csgo\\characters\\models" );
			}
			else
			{
				game_root += "\\csgo\\characters\\models";
			}

			if ( fs::exists( game_root ) )
			{
				std::error_code ec;
				for ( const auto& entry : fs::recursive_directory_iterator( game_root, fs::directory_options::skip_permission_denied, ec ) )
				{
					if ( ec ) break;
					if ( !entry.is_regular_file( ) ) continue;

					const auto ext = entry.path( ).extension( ).string( );
					if ( ext == ".vmdl_c" || ext == ".vmdl" )
					{
						// Skid: same fragment guard for game-shipped models.
						const auto stem = entry.path( ).stem( ).string( );
						if ( stem.empty( ) || detail::is_fragment_model( stem ) || std::isdigit( static_cast< unsigned char >( stem[ 0 ] ) ) )
						{
							continue;
						}

						std::string full_path = entry.path( ).string( );
						auto char_pos = full_path.find( "characters\\" );
						std::string rel_path = ( char_pos != std::string::npos ) ? full_path.substr( char_pos ) : full_path;
						std::replace( rel_path.begin( ), rel_path.end( ), '\\', '/' );

						if ( ext == ".vmdl_c" )
						{
							rel_path = rel_path.substr( 0, rel_path.length( ) - 2 );
						}

						std::string display_name = "[game] " + entry.path( ).stem( ).string( );
						if ( display_name.ends_with( ".vmdl" ) )
						{
							display_name = display_name.substr( 0, display_name.length( ) - 5 );
						}

						this->m_models.push_back( { display_name, rel_path } );
					}
				}
			}
		}

		// Clamp selected index
		if ( settings::g_misc.m_custom_models.selected_index.value >= static_cast<int>( this->m_models.size( ) ) )
		{
			settings::g_misc.m_custom_models.selected_index.value = 0;
		}

		if ( settings::g_misc.m_custom_models.selected_gun_index.value >= static_cast<int>( this->m_gun_models.size( ) ) )
		{
			settings::g_misc.m_custom_models.selected_gun_index.value = 0;
		}

		this->m_last_scan_time = std::chrono::steady_clock::now( );
	}

	void custom_models::update_auto_refresh( )
	{
		const auto now = std::chrono::steady_clock::now( );
		if ( this->m_models.empty( ) || std::chrono::duration_cast<std::chrono::seconds>( now - this->m_last_scan_time ).count() >= 5 )
		{
			this->scan_models( );
		}
	}

	void custom_models::precache_resource( const std::string& path )
	{
		static auto fn_insert = reinterpret_cast<const char*(__fastcall*)(void*, int, const char*, int, bool)>(
			GetProcAddress( reinterpret_cast<HMODULE>( addresses::modules::tier0 ), "?Insert@CBufferString@@QEAAPEBDHPEBDH_N@Z" ) );

		static auto fn_precache = reinterpret_cast<void*(*)(void*, void*, const char*)>(
			memory::resolve_pattern( "resourcesystem.dll:405355574881EC80000000488B01498BE8488BFA" ) );

		const auto irs = reinterpret_cast<void*>( addresses::globals::resource_system );
		if ( !fn_insert || !fn_precache || !irs || path.empty( ) )
		{
			return;
		}

		struct c_buffer_string
		{
			int m_length{ 0 };
			int m_allocated_size{ static_cast<int>( 0x80000000 | 0x40000000 | 8 ) };
			union {
				char* m_string_ptr;
				char m_string_buf[ 8 ];
			};
			c_buffer_string( ) { m_string_ptr = nullptr; }
		} names;

		__try
		{
			fn_insert( &names, 0, path.c_str( ), -1, false );
			fn_precache( irs, &names, "" );
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
		}
	}

	void custom_models::cycle_weapon_owners( std::uintptr_t pawn )
	{
		const auto weapon_services = memory::read<std::uintptr_t>( pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );
		if ( !weapon_services )
		{
			return;
		}

		const auto weapons_base = weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hMyWeapons"_hash );
		const auto weapons_size = memory::read<int>( weapons_base );
		const auto weapons_data = memory::read<std::uintptr_t>( weapons_base + 0x8 );

		if ( !weapons_data || weapons_size <= 0 )
		{
			return;
		}

		for ( auto i = 0; i < weapons_size; ++i )
		{
			const auto handle = memory::read<std::uint32_t>( weapons_data + i * sizeof( std::uint32_t ) );
			const auto weapon = systems::g_entities.lookup( handle );

			if ( !weapon )
			{
				continue;
			}

			const auto owner_off = SCHEMA( "C_BaseEntity", "m_hOwnerEntity"_hash );
			const auto saved_owner = memory::read<std::uint32_t>( weapon + owner_off );

			memory::write<std::uint32_t>( weapon + owner_off, 0xffffffff );
			memory::write<std::uint32_t>( weapon + owner_off, saved_owner );
		}
	}

	void custom_models::on_frame_stage_notify( )
	{
		const auto local = systems::g_local.get( );
		if ( !local.is_alive || systems::g_local.is_in_cinematic( ) || !local.pawn )
		{
			return;
		}

		auto& cfg = settings::g_misc.m_custom_models;
		const auto enabled = cfg.enabled.value;
		const auto selected_idx = cfg.selected_index.value;

		// 1. Character / Player Model
		__try
		{
			const auto game_scene_node = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
			if ( game_scene_node )
			{
				const auto model_state = game_scene_node + SCHEMA( "CSkeletonInstance", "m_modelState"_hash );

				if ( this->m_tracked_pawn != local.pawn )
				{
					this->m_original_model.clear( );
					this->m_applied_model.clear( );
					this->m_applied_gun_model.clear( );
					this->m_applied_weapon_handle = 0;
					this->m_overridden = false;
					this->m_applied_handle = 0;
					this->m_tracked_pawn = local.pawn;
				}

				// If character model disabled or default (0)
				if ( !enabled || selected_idx <= 0 || selected_idx >= static_cast<int>( this->m_models.size( ) ) )
				{
					if ( this->m_overridden && !this->m_original_model.empty( ) )
					{
						memory::call<void>( PATTERN( patterns::set_player_model ), local.pawn, this->m_original_model.c_str( ) );
						this->cycle_weapon_owners( local.pawn );

						this->m_applied_handle = 0;
						this->m_applied_model.clear( );
						this->m_overridden = false;
					}
				}
				else
				{
					const auto& target_model = this->m_models[ selected_idx ].path;
					if ( !target_model.empty( ) && this->m_applied_model != target_model )
					{
						// Save original model before overriding
						if ( !this->m_overridden && this->m_original_model.empty( ) )
						{
							const auto model_name_ptr = memory::read<std::uintptr_t>( model_state + SCHEMA( "CModelState", "m_ModelName"_hash ) );
							if ( model_name_ptr && model_name_ptr > 0x10000 )
							{
								this->m_original_model = memory::read_string( model_name_ptr );
							}
						}

						// Precache custom model first
						this->precache_resource( target_model );

						// Apply model
						memory::call<void>( PATTERN( patterns::set_player_model ), local.pawn, target_model.c_str( ) );

						this->cycle_weapon_owners( local.pawn );

						this->m_applied_model = target_model;
						this->m_overridden = true;
					}
				}
			}
		}
		__except ( EXCEPTION_EXECUTE_HANDLER )
		{
		}

		// 2. Gun / Weapon Models
		const auto gun_enabled = cfg.gun_models_enabled.value;
		const auto selected_gun_idx = cfg.selected_gun_index.value;

		if ( !gun_enabled || selected_gun_idx <= 0 || selected_gun_idx >= static_cast<int>( this->m_gun_models.size( ) ) )
		{
			this->m_applied_gun_model.clear( );
			this->m_applied_weapon_handle = 0;
		}
		else
		{
			__try
			{
				const auto weapon_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pWeaponServices"_hash ) );
				if ( !weapon_services )
				{
					return;
				}

				const auto active_handle = memory::read<std::uint32_t>( weapon_services + SCHEMA( "CPlayer_WeaponServices", "m_hActiveWeapon"_hash ) );
				if ( !active_handle )
				{
					return;
				}

				const auto active_weapon = systems::g_entities.lookup( active_handle );
				if ( !active_weapon )
				{
					return;
				}

				const auto& target_gun = this->m_gun_models[ selected_gun_idx ].path;
				if ( target_gun.empty( ) )
				{
					return;
				}

				// Only apply once per weapon / model change
				if ( this->m_applied_weapon_handle != active_handle || this->m_applied_gun_model != target_gun )
				{
					const auto weapon_scene_node = memory::read<std::uintptr_t>( active_weapon + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
					if ( !weapon_scene_node )
					{
						return;
					}

					// Precache target model
					this->precache_resource( target_gun );

					// Apply model to active weapon entity (world model)
					memory::call<void>( PATTERN( patterns::set_player_model ), active_weapon, target_gun.c_str( ) );

					// Apply model to active viewmodel (first person)
					const auto view_model = features::changer::g_knives.find_hud_model_weapon( local.pawn );
					if ( view_model )
					{
						memory::call<void>( PATTERN( patterns::set_player_model ), view_model, target_gun.c_str( ) );

						const auto view_model_scene_node = memory::read<std::uintptr_t>( view_model + SCHEMA( "C_BaseEntity", "m_pGameSceneNode"_hash ) );
						if ( view_model_scene_node )
						{
							memory::call<void>( PATTERN( patterns::weapon_set_mesh_group_mask ), view_model_scene_node, std::uint64_t{ 1 } );
						}
					}

					memory::call<void>( PATTERN( patterns::weapon_update_skin ), active_weapon, true );
					if ( view_model )
					{
						memory::call<void>( PATTERN( patterns::weapon_update_skin ), view_model, true );
					}

					this->m_applied_weapon_handle = active_handle;
					this->m_applied_gun_model = target_gun;
				}
			}
			__except ( EXCEPTION_EXECUTE_HANDLER )
			{
			}
		}
	}

} // namespace features::misc
