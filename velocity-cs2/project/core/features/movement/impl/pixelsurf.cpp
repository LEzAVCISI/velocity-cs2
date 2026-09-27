#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>

#include "../movement.hpp"
#include <protection/game_addresses.hpp>

namespace features::movement {

	namespace {

		// velocity slipping window constants (shared with edgebug)
		constexpr float k_velocity_lower = -8.293f;
		constexpr float k_velocity_upper = -5.629f;

		// pixel surface detection step size
		constexpr float k_pixel_trace_step = 0.5f;
		constexpr float k_pixel_trace_depth = 4.0f;

	} // namespace

	// ================================================================
	// detect_pixel_surface
	//
	// traces downward from the player's predicted position with small
	// lateral offsets to detect a pixel-walk edge. a pixel-walk occurs
	// when the player stands on a surface that is only a few units wide,
	// typically where two brushes meet at an angle.
	//
	// returns true if we detect a pixel-walkable edge configuration.
	// ================================================================
	bool pixelsurf::detect_pixel_surface(
		const math::vector3& origin,
		const math::vector3& velocity,
		const systems::tracing::bbox_collision& bbox,
		const systems::tracing::player_movement_filter& filter,
		std::uintptr_t movement_services,
		float sv_standable_normal )
	{
		// compute velocity direction for perpendicular offset
		const auto vel2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );
		if ( vel2d < 0.01f )
		{
			return false;
		}

		const float dir_x = velocity.x / vel2d;
		const float dir_y = velocity.y / vel2d;
		const float perp_x = -dir_y;
		const float perp_y = dir_x;

		// trace directly below current position
		const auto center_start = math::vector3{ origin.x, origin.y, origin.z + 2.0f };
		const auto center_end = math::vector3{ origin.x, origin.y, origin.z - k_pixel_trace_depth };
		const auto center_result = systems::g_tracing.trace_player_bbox( center_start, center_end, bbox, filter, movement_services );

		if ( center_result.fraction >= 1.0f || center_result.normal.z < sv_standable_normal )
		{
			return false;
		}

		// trace slightly forward along velocity direction
		const auto fwd_x = origin.x + dir_x * k_pixel_trace_step;
		const auto fwd_y = origin.y + dir_y * k_pixel_trace_step;
		const auto fwd_start = math::vector3{ fwd_x, fwd_y, origin.z + 2.0f };
		const auto fwd_end = math::vector3{ fwd_x, fwd_y, origin.z - k_pixel_trace_depth };
		const auto fwd_result = systems::g_tracing.trace_player_bbox( fwd_start, fwd_end, bbox, filter, movement_services );

		// if forward trace misses ground, we're on an edge
		const bool forward_has_ground = ( fwd_result.fraction < 1.0f && fwd_result.normal.z >= sv_standable_normal );

		// check left and right sides
		const float side_offset = std::max( 2.0f, std::max( std::fabsf( bbox.maxs.x ), std::fabsf( bbox.maxs.y ) ) * 0.5f );

		const auto left_start = math::vector3{ origin.x - perp_x * side_offset, origin.y - perp_y * side_offset, origin.z + 2.0f };
		const auto left_end = math::vector3{ origin.x - perp_x * side_offset, origin.y - perp_y * side_offset, origin.z - k_pixel_trace_depth };
		const auto left_result = systems::g_tracing.trace_player_bbox( left_start, left_end, bbox, filter, movement_services );
		const bool left_has_ground = ( left_result.fraction < 1.0f && left_result.normal.z >= sv_standable_normal );

		const auto right_start = math::vector3{ origin.x + perp_x * side_offset, origin.y + perp_y * side_offset, origin.z + 2.0f };
		const auto right_end = math::vector3{ origin.x + perp_x * side_offset, origin.y + perp_y * side_offset, origin.z - k_pixel_trace_depth };
		const auto right_result = systems::g_tracing.trace_player_bbox( right_start, right_end, bbox, filter, movement_services );
		const bool right_has_ground = ( right_result.fraction < 1.0f && right_result.normal.z >= sv_standable_normal );

		// pixel-walk condition: ground below us, but at least one side has no ground
		if ( !left_has_ground || !right_has_ground || !forward_has_ground )
		{
			// determine edge direction
			if ( !left_has_ground && right_has_ground )
			{
				this->m_edge_direction = 1; // edge is to the left, lean right
			}
			else if ( left_has_ground && !right_has_ground )
			{
				this->m_edge_direction = 0; // edge is to the right, lean left
			}

			return true;
		}

		return false;
	}

	// ================================================================
	// apply_angle_correction (sub_3A24B0)
	//
	// clamps and writes corrected view angles to the command.
	// adjusts yaw to keep the player aligned with the pixel surface.
	// ================================================================
	void pixelsurf::apply_angle_correction(
		systems::input::usercmd* cmd,
		const math::vector3& velocity,
		float target_yaw )
	{
		auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( !base )
		{
			return;
		}

		// clamp angles
		auto pitch = this->m_target_pitch;
		if ( pitch > 89.0f ) pitch = 89.0f;
		if ( pitch < -89.0f ) pitch = -89.0f;

		auto yaw = target_yaw;
		while ( yaw > 180.0f ) yaw -= 360.0f;
		while ( yaw < -180.0f ) yaw += 360.0f;

		// write the corrected view angles
		if ( const auto va = base->mutable_viewangles( ) )
		{
			va->set_x( pitch );
			va->set_y( yaw );
			va->set_z( 0.0f );
		}
	}

	// ================================================================
	// on_create_move — main pixel surf assist entry (sub_1A29A0)
	//
	// called per-tick when pixelsurf is enabled. detects pixel-walk
	// surfaces via downward traces and applies corrective button
	// inputs and view angles to maintain the pixel walk.
	// ================================================================
	void pixelsurf::on_create_move( systems::input::usercmd* cmd )
	{
		this->m_active_this_tick = false;

		if ( !settings::g_movement.pixelsurf.value )
		{
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.pawn )
		{
			return;
		}

		const auto move_type = memory::read<std::uint8_t>( local.pawn + SCHEMA( "C_BaseEntity", "m_nActualMoveType"_hash ) );
		if ( move_type == cstypes::move_type::ladder || move_type == cstypes::move_type::noclip || move_type == cstypes::move_type::none )
		{
			return;
		}

		const auto& prestate = systems::g_prediction.pre( );

		// only assist when on ground or very close to it
		if ( !( prestate.flags & cstypes::entity_flags::on_ground ) )
		{
			// check if we're in the slipping window (edge transition)
			const auto vel_z = prestate.velocity.z;
			if ( vel_z < k_velocity_lower || vel_z > k_velocity_upper )
			{
				this->m_is_slipping = false;
				return;
			}

			this->m_is_slipping = true;
		}

		const auto movement_services = memory::read<std::uintptr_t>( local.pawn + SCHEMA( "C_BasePlayerPawn", "m_pMovementServices"_hash ) );
		if ( !movement_services )
		{
			return;
		}

		const auto collision = local.pawn + SCHEMA( "C_BaseModelEntity", "m_Collision"_hash );
		const auto mins = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMins"_hash ) );
		const auto maxs = memory::read<math::vector3>( collision + SCHEMA( "CCollisionProperty", "m_vecMaxs"_hash ) );

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
		const auto sv_standable_normal = CONVAR ("sv_standable_normal")->get<float>( );

		const auto bbox = systems::tracing::bbox_collision{ mins, maxs };

		if ( !detect_pixel_surface( prestate.networked_origin, prestate.networked_velocity, bbox, filter, movement_services, sv_standable_normal ) )
		{
			this->m_is_slipping = false;
			this->m_edge_type = 0;
			this->m_edge_direction = 0;
			return;
		}

		this->m_active_this_tick = true;
		this->m_last_vel_z = prestate.velocity.z;

		// ---- apply button flags ----
		auto& buttons = cmd->buttons;

		// clear conflicting jump button and ensure in_jump is set
		buttons.value &= ~cstypes::command_buttons::in_jump;
		buttons.value |= cstypes::command_buttons::in_jump;

		// ---- apply angle correction ----
		// compute the yaw that keeps us aligned with the pixel surface
		const auto vel2d = prestate.networked_velocity.length_2d( );
		if ( vel2d > 1.0f )
		{
			// target yaw is the velocity direction, adjusted by edge direction
			float vel_yaw = std::atan2f( prestate.networked_velocity.y, prestate.networked_velocity.x ) * ( 180.0f / 3.14159265f );

			// apply small correction based on which side the edge is
			const float correction = ( this->m_edge_direction == 0 ) ? -2.0f : 2.0f;
			this->m_target_yaw = vel_yaw + correction;

			apply_angle_correction( cmd, prestate.networked_velocity, this->m_target_yaw );
		}

		// zero out movement to prevent falling off
		auto base = cmd->csgo_user_cmd.mutable_base( );
		if ( base )
		{
			base->set_forwardmove( 0.0f );
			base->set_leftmove( 0.0f );
			base->set_upmove( 0.0f );
		}
	}

} // namespace features::movement
