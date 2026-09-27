#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include <protection/game_addresses.hpp>

namespace features::movement {

	namespace {

		// velocity slipping window constants (from sub_19D7F0)
		constexpr float k_velocity_lower = -8.293f;
		constexpr float k_velocity_upper = -5.629f;

		[[nodiscard]] bool side_has_standable_ground( const math::vector3& at_xy_z_reference, float offset_x, float offset_y, const systems::tracing::bbox_collision& bbox, const systems::tracing::player_movement_filter& filter, std::uintptr_t movement_services, float sv_standable_normal )
		{
			const auto start = math::vector3{ at_xy_z_reference.x + offset_x, at_xy_z_reference.y + offset_y, at_xy_z_reference.z + 2.0f };
			const auto end = math::vector3{ at_xy_z_reference.x + offset_x, at_xy_z_reference.y + offset_y, at_xy_z_reference.z - 4.0f };

			const auto r = systems::g_tracing.trace_player_bbox( start, end, bbox, filter, movement_services );
			return r.fraction > 0.0f && r.fraction < 1.0f && r.normal.z >= sv_standable_normal;
		}

		[[nodiscard]] bool mode_allows( int mode, bool holding_jump, float vel2d, float vz )
		{
			switch ( mode )
			{
			case 0:
				return true;
			case 1:
				return true;
			case 2:
				return !holding_jump;
			case 3:
				return vel2d > 15.0f;
			case 4:
				return vel2d > 25.0f && vz < -100.0f;
			default:
				return true;
			}
		}

		[[nodiscard]] bool edge_geometry_ok( int mode, bool left_ok, bool right_ok )
		{
			if ( mode == 0 )
			{
				return true;
			}

			if ( left_ok && right_ok )
			{
				return false;
			}

			return true;
		}

		// ================================================================
		// velocity-window detection (sub_19D7F0)
		//
		// checks if the player is in the slipping velocity window while
		// airborne. returns true when:
		//   1. not on ground
		//   2. falling (velocity.z < 0)
		//   3. velocity.z ∈ [k_velocity_lower, k_velocity_upper]
		// ================================================================
		[[nodiscard]] bool detect_velocity_window( std::uintptr_t pawn )
		{
			const auto flags = memory::read<std::uint32_t>( pawn + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) );
			if ( flags & cstypes::entity_flags::on_ground )
			{
				return false;
			}

			const auto velocity = memory::read<math::vector3>( pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
			if ( velocity.z >= 0.0f )
			{
				return false;
			}

			return ( velocity.z >= k_velocity_lower && velocity.z <= k_velocity_upper );
		}

		// ================================================================
		// prediction-based lookahead assist (sub_19CFB0)
		//
		// uses the engine prediction system to simulate up to `radius`
		// ticks ahead, checking each simulated tick for the velocity
		// slipping window. runs two passes:
		//   pass 0: no duck held (clear IN_DUCK)
		//   pass 1: duck held (set IN_DUCK)
		// ================================================================
		[[nodiscard]] bool try_prediction_lookahead(
			systems::input::usercmd* cmd,
			const systems::local::snapshot& local,
			int radius )
		{
			const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
			if ( !movement_services )
			{
				return false;
			}

			bool found = false;
			int found_tick = 0;

			// we simulate inside the prediction system's state guard,
			// so all pawn state is restored after the check
			for ( int pass = 0; pass < 2 && !found; ++pass )
			{
				auto sim_result = systems::g_prediction.simulate( cmd, local, [ & ]( )
				{
					for ( int i = 0; i < radius; ++i )
					{
						const auto flags = memory::read<std::uint32_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) );
						if ( flags & cstypes::entity_flags::on_ground )
						{
							break;
						}

						const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
						if ( move_type == cstypes::move_type::ladder )
						{
							break;
						}

						const auto velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
						if ( velocity.z < 0.0f &&
							velocity.z >= k_velocity_lower &&
							velocity.z <= k_velocity_upper )
						{
							found = true;
							found_tick = i + 1;
							break;
						}
					}
				} );

				if ( !sim_result )
				{
					break;
				}

				if ( found )
				{
					break;
				}

				// for pass 1, set duck in the command buttons before re-simulating
				if ( pass == 0 )
				{
					cmd->buttons.value |= cstypes::command_buttons::in_duck;
				}
			}

			return found;
		}

	} // namespace

	void edgebug::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;

		if ( !settings::g_movement.edgebug.value )
		{
			return;
		}

		const auto mode = std::clamp( settings::g_movement.edgebug_mode.value, 0, 4 );
		const auto passes = std::clamp( settings::g_movement.edgebug_passes.value, 1, 5 );

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );
		if ( prestate.flags & cstypes::entity_flags::on_ground )
		{
			return;
		}

		if ( prestate.networked_velocity.z > 0.0f )
		{
			return;
		}

		const auto holding_jump = ( cmd->buttons.value & cstypes::command_buttons::in_jump ) != 0;
		const auto vel2d = prestate.networked_velocity.length_2d( );

		if ( !mode_allows( mode, holding_jump, vel2d, prestate.networked_velocity.z ) )
		{
			return;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		// ---- prediction-based velocity window check ----
		// check if we're already in the slipping window, or if the
		// prediction lookahead finds us entering it within N ticks
		const bool in_window_now = detect_velocity_window( local.pawn );
		bool assist_triggered = in_window_now;

		if ( !assist_triggered && settings::g_movement.edgebug_assist.value )
		{
			const auto radius = std::clamp( settings::g_movement.edgebug_assist_radius.value, 1, 128 );
			assist_triggered = try_prediction_lookahead( cmd, local, radius );
		}

		if ( assist_triggered )
		{
			this->m_assist_active = true;
			++this->m_assist_ticks;
		}
		else
		{
			this->m_assist_active = false;
			this->m_assist_ticks = 0;
		}

		// ---- trace-based edge detection (original logic) ----
		const auto duck_amount = memory::read<float>( movement_services + SCHEMA( "CCSPlayer_MovementServices", "m_flDuckAmount"_hash ) );
		const auto holding_duck = ( cmd->buttons.value & cstypes::command_buttons::in_duck ) != 0;
		const auto mins = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
		auto maxs = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash ) + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );

		auto trace_origin = prestate.networked_origin;
		{
			if ( holding_duck && duck_amount > 0.0f )
			{
				const auto standing_height{ 72.0f };
				const auto duck_hull_diff = standing_height - maxs.z;
				trace_origin.z -= duck_hull_diff * 0.5f;
				maxs.z = standing_height;
			}
		}

		auto trace_mask{ 0ull };
		{
			const auto pawn_ptr = memory::read<std::uintptr_t>( movement_services + 56 );
			trace_mask = memory::read<std::uintptr_t>( pawn_ptr + 0xd48 );

			if ( !pawn_ptr || ( memory::read<std::uint32_t>( pawn_ptr + 0x3f8 ) & 0x10 ) )
			{
				trace_mask |= 0x20;
			}
		}

		const auto filter = systems::g_tracing.make_player_movement_filter( local.pawn, trace_mask, 11 );
		const auto sv_gravity = CONVAR ("sv_gravity")->get<float>( );
		const auto sv_standable_normal = CONVAR ("sv_standable_normal")->get<float>( );
		const auto gravity_scale = memory::read<float>( local.pawn + SCHEMA( "C_BaseEntity", "m_flGravityScale"_hash ) );

		auto velocity = prestate.networked_velocity;
		velocity.z -= ( gravity_scale * sv_gravity * cstypes::tick_interval ) * 0.5f;

		math::vector3 trace_start = trace_origin;
		math::vector3 trace_end{};

		trace_end.x = trace_origin.x + velocity.x * cstypes::tick_interval;
		trace_end.y = trace_origin.y + velocity.y * cstypes::tick_interval;
		trace_end.z = trace_origin.z + velocity.z * cstypes::tick_interval;

		trace_end.z -= 2.0f;

		const auto bbox = systems::tracing::bbox_collision{ mins, maxs };
		const auto result = systems::g_tracing.trace_player_bbox( trace_start, trace_end, bbox, filter, movement_services );
		const auto valid_trace = ( result.fraction > 0.0f && result.fraction < 1.0f );

		// allow activation either from trace hit or from assist prediction
		if ( !valid_trace || result.normal.z < sv_standable_normal )
		{
			if ( !assist_triggered )
			{
				return;
			}

			// assist triggered but no trace hit: still inject duck subticks
			// using a default fraction to give the engine the best chance
			this->m_active_this_tick = true;

			const auto base = cmd->csgo_user_cmd.mutable_base( );
			if ( !base )
			{
				return;
			}

			const auto subtick_moves = base->mutable_subtick_moves( );
			const float when = 0.5f; // midpoint of tick

			const float seg = when / static_cast< float >( passes );
			for ( int p = 0; p < passes; ++p )
			{
				const float t_down = seg * static_cast< float >( p );
				const float t_up = seg * static_cast< float >( p + 1 );

				if ( const auto duck_down = systems::g_input.acquire_subtick_step( subtick_moves ) )
				{
					duck_down->set_button( cstypes::command_buttons::in_duck );
					duck_down->set_pressed( true );
					duck_down->set_when( t_down );
					duck_down->set_analog_forward_delta( 0.0f );
					duck_down->set_analog_left_delta( 0.0f );
				}

				if ( const auto duck_up = systems::g_input.acquire_subtick_step( subtick_moves ) )
				{
					duck_up->set_button( cstypes::command_buttons::in_duck );
					duck_up->set_pressed( false );
					duck_up->set_when( t_up );
					duck_up->set_analog_forward_delta( 0.0f );
					duck_up->set_analog_left_delta( 0.0f );
				}
			}

			return;
		}

		const auto dot = velocity.x * result.normal.x + velocity.y * result.normal.y;
		if ( !( result.normal.z >= 0.98f || dot >= 0.0f ) )
		{
			return;
		}

		const auto vel2d_len = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );
		float perp_x{};
		float perp_y{};
		if ( vel2d_len > 0.01f )
		{
			perp_x = -velocity.y / vel2d_len;
			perp_y = velocity.x / vel2d_len;
		}
		else
		{
			perp_x = 1.0f;
			perp_y = 0.0f;
		}

		const auto side_dist = std::max( 14.0f, std::max( std::fabsf( maxs.x ), std::fabsf( maxs.y ) ) * 1.25f );
		const math::vector3 ground_ref{ result.position.x, result.position.y, trace_origin.z };

		const auto left_ok = side_has_standable_ground( ground_ref, -perp_x * side_dist, -perp_y * side_dist, bbox, filter, movement_services, sv_standable_normal );
		const auto right_ok = side_has_standable_ground( ground_ref, perp_x * side_dist, perp_y * side_dist, bbox, filter, movement_services, sv_standable_normal );

		if ( !edge_geometry_ok( mode, left_ok, right_ok ) )
		{
			return;
		}

		const auto when = std::clamp( result.fraction, 0.001f, 0.99f );

		const auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		this->m_active_this_tick = true;

		const auto subtick_moves = base->mutable_subtick_moves( );

		// Multiple passes = chained duck tap windows across [0, when] (not duplicate pulses at one time).
		const float seg = when / static_cast< float >( passes );
		for ( int p = 0; p < passes; ++p )
		{
			const float t_down = seg * static_cast< float >( p );
			const float t_up = seg * static_cast< float >( p + 1 );

			if ( const auto duck_down = systems::g_input.acquire_subtick_step( subtick_moves ) )
			{
				duck_down->set_button( cstypes::command_buttons::in_duck );
				duck_down->set_pressed( true );
				duck_down->set_when( t_down );
				duck_down->set_analog_forward_delta( 0.0f );
				duck_down->set_analog_left_delta( 0.0f );
			}

			if ( const auto duck_up = systems::g_input.acquire_subtick_step( subtick_moves ) )
			{
				duck_up->set_button( cstypes::command_buttons::in_duck );
				duck_up->set_pressed( false );
				duck_up->set_when( t_up );
				duck_up->set_analog_forward_delta( 0.0f );
				duck_up->set_analog_left_delta( 0.0f );
			}
		}

		if ( settings::g_movement.edgebug_include_jump_steps.value && holding_jump )
		{
			if ( const auto jump_up = systems::g_input.acquire_subtick_step( subtick_moves ) )
			{
				jump_up->set_button( cstypes::command_buttons::in_jump );
				jump_up->set_pressed( false );
				jump_up->set_when( when );
				jump_up->set_analog_forward_delta( 0.0f );
				jump_up->set_analog_left_delta( 0.0f );
			}

			if ( const auto jump_down = systems::g_input.acquire_subtick_step( subtick_moves ) )
			{
				jump_down->set_button( cstypes::command_buttons::in_jump );
				jump_down->set_pressed( true );
				jump_down->set_when( when );
				jump_down->set_analog_forward_delta( 0.0f );
				jump_down->set_analog_left_delta( 0.0f );
			}
		}
	}

	void edgebug::on_render( xdraw::draw_list& draw_list )
	{
		( void )draw_list;

		if ( !settings::g_movement.edgebug.value )
		{
			return;
		}
	}

} // namespace features::movement
