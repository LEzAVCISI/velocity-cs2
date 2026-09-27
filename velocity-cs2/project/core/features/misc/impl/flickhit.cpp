#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>
#include <protection/game_addresses.hpp>

#include "../misc.hpp"

namespace features::misc {

	namespace
	{
		void __fastcall flickhit_death_thunk( void* event )
		{
			g_flickhit.handle_death_event( reinterpret_cast< std::uintptr_t >( event ) );
		}
	} // namespace

	void flickhit::note_shot_at( std::uintptr_t pawn )
	{
		if ( !settings::g_misc.m_flickhit.enabled.value || !pawn )
		{
			return;
		}

		this->m_pending_pawn = pawn;
		this->m_pending_since_ms = GetTickCount64( );
	}

	void flickhit::start_flick( )
	{
		this->m_base_yaw = systems::g_input.get_view_angles( ).y;
		this->m_flick_start_ms = GetTickCount64( );
		this->m_direction = -this->m_direction;
		this->m_flicking = true;
		this->m_pending_pawn = 0;
	}

	void flickhit::sync_event_registration( )
	{
		const auto want = settings::g_misc.m_flickhit.enabled.value && settings::g_misc.m_flickhit.use_events.value;

		if ( want && !this->m_events_on )
		{
			this->m_events_on = systems::events::register_listener( "player_death", &flickhit_death_thunk );
		}
		else if ( !want && this->m_events_on )
		{
			systems::events::unregister_listener( "player_death" );
			this->m_events_on = false;
		}
	}

	void flickhit::handle_death_event( std::uintptr_t event )
	{
		if ( !event || !settings::g_misc.m_flickhit.enabled.value )
		{
			return;
		}

		const auto attacker_key = cstypes::event_hash{ 0, "attacker" };
		const auto attacker = memory::call<std::uintptr_t>( PATTERN( patterns::game_event_get_controller ), event, &attacker_key );
		if ( !attacker )
		{
			return;
		}

		// Only our own kills flick — teammates' kills are ignored.
		if ( attacker != systems::g_local.get( ).controller )
		{
			return;
		}

		this->start_flick( );
	}

	void flickhit::on_create_move( )
	{
		this->sync_event_registration( );

		const auto now_ms = GetTickCount64( );

		// Confirm the pending shot: target dead (HP <= 0) shortly after we
		// fired at it. Anything else (gone/unreadable/stale) just clears.
		if ( this->m_pending_pawn )
		{
			auto dead{ false };
			auto gone{ false };

			if ( !systems::g_entities.exists( this->m_pending_pawn ) )
			{
				gone = true;
			}
			else if ( const auto hp = memory::safe_read<int>( this->m_pending_pawn + SCHEMA( "C_BaseEntity", "m_iHealth"_hash ) ) )
			{
				dead = *hp <= 0;
			}
			else
			{
				gone = true;
			}

			if ( dead )
			{
				if ( settings::g_misc.m_flickhit.enabled.value )
				{
					this->start_flick( );
				}
				else
				{
					this->m_pending_pawn = 0;
				}
			}
			else if ( gone || now_ms - this->m_pending_since_ms > k_confirm_window_ms )
			{
				this->m_pending_pawn = 0;
			}
		}

		if ( !this->m_flicking )
		{
			return;
		}

		if ( !settings::g_misc.m_flickhit.enabled.value )
		{
			this->m_flicking = false;
			return;
		}

		const auto elapsed = now_ms - this->m_flick_start_ms;
		const auto stay = settings::g_misc.m_flickhit.flick_mode.value == settings::misc::flickhit::mode::stay;
		const auto full_offset = static_cast< float >( settings::g_misc.m_flickhit.angle.value ) * this->m_direction;

		if ( elapsed >= k_flick_ms )
		{
			if ( stay )
			{
				// Stay: bake the final yaw exactly, then hands off. The camera
				// stays where the flick put it.
				auto cam = systems::g_input.get_view_angles( );
				cam.y = this->m_base_yaw + full_offset;
				math::helpers::normalize_angle( cam.y );
				systems::g_input.set_view_angles( cam );
			}
			// Snap back ends at ~0 offset by design; leave the camera alone
			// so we never fight mouse input applied mid-flick.
			this->m_flicking = false;
			return;
		}

		const auto t = static_cast< float >( elapsed ) / static_cast< float >( k_flick_ms );
		// Stay: eased ramp straight to the final angle (0 -> angle).
		// Snap back: fast out-and-back kick (0 -> angle -> 0, ~100 ms).
		const auto curve = stay
			? 1.0f - ( 1.0f - t ) * ( 1.0f - t )
			: std::sinf( t * std::numbers::pi_v<float> );
		const auto offset = static_cast< float >( settings::g_misc.m_flickhit.angle.value ) * curve * this->m_direction;

		auto cam = systems::g_input.get_view_angles( );
		cam.y = this->m_base_yaw + offset;
		math::helpers::normalize_angle( cam.y );
		systems::g_input.set_view_angles( cam );
	}

} // namespace features::misc
