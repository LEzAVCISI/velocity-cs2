#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/features/combat/resolver.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	void resolver::on_create_move( systems::input::usercmd* /*cmd*/ )
	{
		this->update( );
	}

	void resolver::update( )
	{
		const auto global_vars = memory::read<std::uintptr_t>( addresses::globals::global_vars );
		if ( !global_vars )
			return;

		const auto cur_time = memory::read<float>( global_vars + 0x30 );
		const auto local = systems::g_local.get( );
		const auto players = systems::g_entities.get_by_type( systems::entities::type::player );

		std::unordered_set<std::uintptr_t> active{};
		active.reserve( players.size( ) );

		for ( const auto& p : players )
		{
			if ( !p.ptr || p.ptr == local.controller )
				continue;
			if ( !memory::read<bool>( p.ptr + SCHEMA( "CCSPlayerController", "m_bPawnIsAlive"_hash ) ) )
				continue;
			const auto pawn_handle = memory::read<std::uint32_t>( p.ptr + SCHEMA( "CBasePlayerController", "m_hPawn"_hash ) );
			const auto pawn = systems::g_entities.lookup( pawn_handle );
			if ( !pawn || pawn == local.pawn )
				continue;
			const auto team = memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iTeamNum"_hash ) );
			if ( !local.is_this_other_team( team ) )
				continue;
			if ( memory::read<int>( pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) <= 0 )
				continue;

			active.insert( pawn );
			this->update_player( pawn, cur_time );
		}

		// prune dead / dormant
		std::unique_lock lock( m_mtx );
		std::erase_if( m_states, [ & ]( const auto& kv ) { return !active.contains( kv.first ); } );
	}

	void resolver::update_player( std::uintptr_t pawn, float cur_time )
	{
		const auto eye_angles = memory::read<math::vector3>( pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_hash ) );
		const auto velocity = memory::read<math::vector3>( pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
		const auto flags = memory::read<std::uint32_t>( pawn + SCHEMA( "C_BaseEntity", "m_fFlags"_hash ) );

		const auto speed2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );
		const bool moving = speed2d > 5.0f || ( flags & cstypes::entity_flags::on_ground ) == 0;

		float confidence{ 1.0f };
		float body_yaw_delta{ 0.0f };
		bool has_anim_info = this->try_read_anim_layers( pawn, body_yaw_delta );

		float detected_side_confidence{ 1.0f };
		auto detected_side = this->detect_desync_side( pawn, eye_angles.y, velocity, detected_side_confidence );

		// If we got anim layer info, boost confidence accordingly
		if ( has_anim_info )
		{
			if ( body_yaw_delta > 35.0f )
				detected_side = side::right;
			else if ( body_yaw_delta < -35.0f )
				detected_side = side::left;
			confidence = 0.85f;
		}
		else if ( moving )
		{
			// Moving players can't have large desync; trust eye yaw
			detected_side = side::zero;
			confidence = 1.0f;
		}
		else
		{
			confidence = detected_side_confidence;
		}

		std::unique_lock lock( m_mtx );
		auto& st = m_states[ pawn ];
		st.pawn = pawn;
		st.last_eye_yaw = st.eye_yaw;
		st.eye_yaw = eye_angles.y;
		st.is_moving = moving;
		st.last_update_time = cur_time;
		st.is_valid = true;

		// Don't overwrite brute-forced side if we are in miss cycle and confidence is low
		if ( st.miss_count > 0 && st.confidence < 0.5f )
		{
			// keep brute side until hit confirms
		}
		else
		{
			st.current_side = detected_side;
			st.confidence = confidence;
		}

		// Resolved yaw is eye yaw + side bias (58 deg is typical max desync in CS2)
		switch ( st.current_side )
		{
		case side::left:  st.resolved_yaw = std::remainderf( eye_angles.y - 58.0f, 360.0f ); break;
		case side::right: st.resolved_yaw = std::remainderf( eye_angles.y + 58.0f, 360.0f ); break;
		default:          st.resolved_yaw = eye_angles.y; break;
		}
	}

	resolver::side resolver::detect_desync_side( std::uintptr_t /*pawn*/, float eye_yaw, const math::vector3& velocity, float& out_confidence ) const
	{
		const auto speed2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );
		if ( speed2d < 1.0f )
		{
			out_confidence = 0.35f;
			return side::zero;
		}

		const auto move_yaw = std::atan2f( velocity.y, velocity.x ) * 180.0f / std::numbers::pi_v<float>;
		auto delta = std::remainderf( eye_yaw - move_yaw, 360.0f );

		// If moving and eye yaw aligns with move dir, no desync
		if ( std::fabsf( delta ) < 35.0f )
		{
			out_confidence = 0.9f;
			return side::zero;
		}

		out_confidence = 0.55f;
		// Use velocity delta direction to guess side
		return delta > 0 ? side::right : side::left;
	}

	bool resolver::try_read_anim_layers( std::uintptr_t pawn, float& out_body_yaw_delta ) const
	{
		// Try several possible schema paths for anim overlay / pose.
		// If none resolve, fallback to false and let velocity-based logic handle it.
		// This keeps resolver functional even if offsets shift after updates.
		static int s_anim_overlay_off = -1;
		static int s_body_yaw_off = -1;
		static bool s_tried = false;

		if ( !s_tried )
		{
			s_tried = true;
			// Discover from schema system; 0 means not found
			s_anim_overlay_off = static_cast<int>( systems::schemas::lookup( "C_BaseEntity", "m_AnimOverlay"_hash ) );
			if ( s_anim_overlay_off == 0 )
				s_anim_overlay_off = static_cast<int>( systems::schemas::lookup( "C_CSPlayerPawnBase", "m_AnimOverlay"_hash ) );
			if ( s_anim_overlay_off == 0 )
				s_anim_overlay_off = static_cast<int>( systems::schemas::lookup( "C_CSPlayerPawn", "m_AnimOverlay"_hash ) );

			s_body_yaw_off = static_cast<int>( systems::schemas::lookup( "C_CSPlayerPawnBase", "m_flBodyYaw"_hash ) );
			if ( s_body_yaw_off == 0 )
				s_body_yaw_off = static_cast<int>( systems::schemas::lookup( "C_CSPlayerPawn", "m_flBodyYaw"_hash ) );
		}

		if ( s_body_yaw_off > 0 )
		{
			const auto body_yaw = memory::read<float>( pawn + s_body_yaw_off );
			const auto eye_yaw = memory::read<math::vector3>( pawn + SCHEMA( "C_CSPlayerPawn", "m_angEyeAngles"_hash ) ).y;
			if ( std::isfinite( body_yaw ) && std::isfinite( eye_yaw ) )
			{
				auto delta = std::remainderf( body_yaw - eye_yaw, 360.0f );
				out_body_yaw_delta = delta;
				return true;
			}
		}

		if ( s_anim_overlay_off > 0 )
		{
			// CAnimationLayer is ~0x40 bytes; layer 7 often holds adjust animation weight.
			// We just probe two layers for activity to infer lean direction.
			const auto overlay = memory::read<std::uintptr_t>( pawn + s_anim_overlay_off );
			if ( overlay )
			{
				// Heuristic: read weight of lean layers (no hard struct needed)
				const auto w1 = memory::safe_read<float>( overlay + 0x40 * 4 + 0x10 ).value_or( 0.0f );
				const auto w2 = memory::safe_read<float>( overlay + 0x40 * 7 + 0x10 ).value_or( 0.0f );
				if ( std::isfinite( w1 ) && std::isfinite( w2 ) && ( w1 > 0.01f || w2 > 0.01f ) )
				{
					out_body_yaw_delta = w1 > w2 ? 40.0f : -40.0f;
					return true;
				}
			}
		}

		return false;
	}

	float resolver::get_resolved_yaw( std::uintptr_t pawn ) const
	{
		std::shared_lock lock( m_mtx );
		auto it = m_states.find( pawn );
		if ( it == m_states.end( ) || !it->second.is_valid )
			return 0.0f;
		return it->second.resolved_yaw;
	}

	resolver::side resolver::get_side( std::uintptr_t pawn ) const
	{
		std::shared_lock lock( m_mtx );
		auto it = m_states.find( pawn );
		if ( it == m_states.end( ) )
			return side::zero;
		return it->second.current_side;
	}

	float resolver::get_confidence( std::uintptr_t pawn ) const
	{
		std::shared_lock lock( m_mtx );
		auto it = m_states.find( pawn );
		if ( it == m_states.end( ) )
			return 0.0f;
		return it->second.confidence;
	}

	bool resolver::is_low_confidence( std::uintptr_t pawn ) const
	{
		return this->get_confidence( pawn ) < 0.5f;
	}

	bool resolver::should_prefer_body( std::uintptr_t pawn ) const
	{
		std::shared_lock lock( m_mtx );
		auto it = m_states.find( pawn );
		if ( it == m_states.end( ) )
			return false;
		return it->second.confidence < 0.45f || it->second.miss_count >= 2;
	}

	float resolver::get_yaw_correction( std::uintptr_t pawn ) const
	{
		std::shared_lock lock( m_mtx );
		auto it = m_states.find( pawn );
		if ( it == m_states.end( ) )
			return 0.0f;
		switch ( it->second.current_side )
		{
		case side::left:  return -58.0f;
		case side::right: return 58.0f;
		default:          return 0.0f;
		}
	}

	void resolver::on_shot_miss( std::uintptr_t pawn )
	{
		std::unique_lock lock( m_mtx );
		auto& st = m_states[ pawn ];
		st.pawn = pawn;
		st.miss_count++;
		st.brute_index = ( st.brute_index + 1 ) % 3;
		st.confidence = std::max( 0.1f, st.confidence - 0.25f );

		// Brute-force flip side
		if ( st.brute_index == 1 )
			st.current_side = side::left;
		else if ( st.brute_index == 2 )
			st.current_side = side::right;
		else
			st.current_side = side::zero;

		switch ( st.current_side )
		{
		case side::left:  st.resolved_yaw = std::remainderf( st.eye_yaw - 58.0f, 360.0f ); break;
		case side::right: st.resolved_yaw = std::remainderf( st.eye_yaw + 58.0f, 360.0f ); break;
		default:          st.resolved_yaw = st.eye_yaw; break;
		}
	}

	void resolver::on_shot_hit( std::uintptr_t pawn )
	{
		std::unique_lock lock( m_mtx );
		auto it = m_states.find( pawn );
		if ( it == m_states.end( ) )
			return;
		it->second.miss_count = 0;
		it->second.brute_index = 0;
		it->second.confidence = std::min( 1.0f, it->second.confidence + 0.35f );
	}

	void resolver::on_player_hurt( std::uintptr_t pawn, bool is_hit )
	{
		if ( is_hit )
			this->on_shot_hit( pawn );
		else
			this->on_shot_miss( pawn );
	}

	void resolver::reset( )
	{
		std::unique_lock lock( m_mtx );
		m_states.clear( );
	}

} // namespace features::combat
