#include <pch/pch.hpp>
#include <utilities/memory/memory.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>
#include <core/features/combat/fakelag.hpp>
#include <protection/game_addresses.hpp>

namespace features::combat {

	void fakelag::on_create_move( systems::input::usercmd* cmd, bool force_send )
	{
		const auto& cfg = settings::g_combat.m_fakelag;
		if ( !cfg.enabled.value || cfg.limit.value <= 0 )
		{
			m_choked = 0;
			m_should_choke = false;
			return;
		}

		const auto local = systems::g_local.get( );
		if ( !local.is_alive || !cmd )
		{
			m_choked = 0;
			m_should_choke = false;
			return;
		}

		// Always send when shooting
		if ( force_send || ( cmd->buttons.value & ( cstypes::command_buttons::in_attack | cstypes::command_buttons::in_second_attack ) ) )
		{
			m_choked = 0;
			m_should_choke = false;
			return;
		}

		const int want = this->desired_choke( local, cmd );
		const int limit = std::clamp( static_cast<int>( cfg.limit.value ), 1, 14 );

		if ( m_choked < std::min( want, limit ) )
		{
			m_should_choke = true;
			m_choked++;
			// Suppress tick: by not updating input history? For now we just flag.
			// Actual choke would need net channel manipulation; we keep count for UI/logic.
		}
		else
		{
			m_should_choke = false;
			m_choked = 0;
		}
	}

	int fakelag::desired_choke( const systems::local::snapshot& local, systems::input::usercmd* /*cmd*/ ) const
	{
		const auto& cfg = settings::g_combat.m_fakelag;
		const int limit = std::clamp( static_cast<int>( cfg.limit.value ), 1, 14 );
		const auto m = cfg.type.value;

		if ( m == settings::combat::fakelag::mode::off )
			return 0;
		if ( m == settings::combat::fakelag::mode::static_ )
			return limit;

		const auto velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
		const auto speed2d = std::sqrtf( velocity.x * velocity.x + velocity.y * velocity.y );

		if ( m == settings::combat::fakelag::mode::dynamic )
		{
			// More choke when moving fast, less when standing for better peek
			if ( speed2d > 250.0f )
				return limit;
			if ( speed2d > 120.0f )
				return std::max( 2, limit - 2 );
			return std::max( 1, limit - 4 );
		}

		if ( m == settings::combat::fakelag::mode::adaptive )
		{
			// Adaptive: increase when not being shot? Simple heuristic based on velocity
			if ( speed2d < 5.0f )
				return std::clamp( limit - 1, 1, 14 );
			return limit;
		}

		return limit;
	}

	void fakelag::reset( )
	{
		m_choked = 0;
		m_should_choke = false;
	}

} // namespace features::combat
