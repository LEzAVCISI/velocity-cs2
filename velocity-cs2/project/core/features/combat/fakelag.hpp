#pragma once
#include <core/systems/systems.hpp>
#include <core/settings.hpp>

namespace features::combat {

	class fakelag
	{
	public:
		void on_create_move( systems::input::usercmd* cmd, bool force_send = false );
		[[nodiscard]] bool should_choke( ) const { return m_should_choke; }
		[[nodiscard]] int get_choked( ) const { return m_choked; }
		void reset( );

	private:
		[[nodiscard]] int desired_choke( const systems::local::snapshot& local, systems::input::usercmd* cmd ) const;

		int m_choked{ 0 };
		bool m_should_choke{ false };
	};

} // namespace features::combat
