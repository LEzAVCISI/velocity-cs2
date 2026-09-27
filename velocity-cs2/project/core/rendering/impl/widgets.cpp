#include <pch/pch.hpp>
#include <utilities/math/math.hpp>
#include <utilities/memory/memory.hpp>
#include <utilities/addresses/addresses.hpp>
#include <core/systems/systems.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

#include "../rendering.hpp"
#include <utilities/security/security.hpp>

namespace rendering {

	void widgets::draw( )
	{
		auto& dl = xdraw::get( );

		if ( settings::g_misc.m_watermark.enabled.value )
		{
			this->watermark( dl );
		}

		this->keybinds( dl );
	}

	void widgets::watermark( xdraw::draw_list& draw_list )
	{
		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		const auto& s  = xui::ctx( ).style;
		const auto& wm = settings::g_misc.m_watermark;
		const auto framerate = xdraw::framerate( );
		const auto local = systems::g_local.get( );

		constexpr auto h{ 22.0f };
		constexpr auto margin{ 10.0f };
		constexpr auto pad_x{ 8.0f };
		constexpr auto sep_spacing{ 6.0f };

		// ── time ────────────────────────────────────────────────────────────
		SYSTEMTIME st{};
		GetLocalTime( &st );
		char time_buf[ 16 ]{};
		std::snprintf( time_buf, sizeof( time_buf ), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond );

		// ── fps ─────────────────────────────────────────────────────────────
		static auto smoothed_fps{ 0.0f };
		if ( smoothed_fps == 0.0f ) smoothed_fps = framerate;
		smoothed_fps += ( framerate - smoothed_fps ) * std::min( 2.0f * xdraw::delta_time( ), 1.0f );
		char fps_val[ 16 ]{};
		std::snprintf( fps_val, sizeof( fps_val ), "%d fps", static_cast<int>( std::round( smoothed_fps ) ) );

		// ── ping ────────────────────────────────────────────────────────────
		auto ping{ 0 };
		if ( local.is_alive && local.controller && systems::g_entities.exists( local.controller ) )
			ping = memory::read<std::uint32_t>( local.controller + SCHEMA( "CCSPlayerController", "m_iPing"_hash ) );
		char ping_val[ 16 ]{};
		std::snprintf( ping_val, sizeof( ping_val ), "%d ms", ping );

		// ── map name ────────────────────────────────────────────────────────
		const bool has_map = wm.show_map.value && !s_map_name.empty( );

		// ── tick rate ───────────────────────────────────────────────────────
		static auto last_server_tick{ 0 };
		static auto last_curtime{ 0.0f };
		static auto measured_tickrate{ 0 };

		if ( local.controller )
		{
			const auto net_for_tick = addresses::globals::network_client_service;
			const auto tick_state   = net_for_tick ? memory::call_vfunc<std::uintptr_t>( net_for_tick, 23 ) : 0;
			const auto server_tick  = tick_state   ? memory::read<int>( tick_state + 892 ) : 0;
			const auto gv           = memory::read<std::uintptr_t>( addresses::globals::global_vars );
			const auto curtime      = gv ? memory::read<float>( gv + 0x30 ) : 0.0f;

			if ( server_tick > 0 && last_server_tick > 0 && curtime - last_curtime >= 2.0f )
			{
				const auto tick_delta = server_tick - last_server_tick;
				const auto time_delta = curtime - last_curtime;
				if ( tick_delta > 0 && time_delta > 0.5f )
				{
					const auto rate = static_cast<int>( std::round( tick_delta / time_delta ) );
					if ( rate >= 16 && rate <= 256 ) measured_tickrate = rate;
				}
				last_server_tick = server_tick;
				last_curtime     = curtime;
			}
			else if ( last_server_tick == 0 && server_tick > 0 )
			{
				last_server_tick = server_tick;
				last_curtime     = curtime;
			}
		}
		else
		{
			last_server_tick = 0;
			last_curtime     = 0.0f;
			measured_tickrate = 0;
		}

		const bool has_tick = wm.show_tick.value && local.controller && measured_tickrate > 0;
		char tick_val[ 16 ]{};
		if ( has_tick ) std::snprintf( tick_val, sizeof( tick_val ), "%d tick", measured_tickrate );

		// ── velocity ────────────────────────────────────────────────────────
		const bool has_velocity = wm.show_velocity.value && local.is_alive && local.pawn;
		static auto smoothed_velocity{ 0.0f };
		char vel_val[ 16 ]{};
		if ( has_velocity )
		{
			const auto velocity = memory::read<math::vector3>( local.pawn + SCHEMA( "C_BaseEntity", "m_vecAbsVelocity"_hash ) );
			const auto speed = velocity.length_2d( );
			smoothed_velocity += ( speed - smoothed_velocity ) * std::min( 8.0f * xdraw::delta_time( ), 1.0f );
			std::snprintf( vel_val, sizeof( vel_val ), "%.0f u/s", smoothed_velocity );
		}
		else
		{
			smoothed_velocity = 0.0f;
		}

		// ── collect watermark segments ──────────────────────────────────────
		struct segment_t
		{
			std::string text{};
			xdraw::color color{};
			xdraw::font* font{ nullptr };
		};

		std::vector<segment_t> segs;
		segs.push_back( { "if3b", s.accent, g_fonts.inter_bold[ fonts::size::normal ] } );

		if ( wm.show_user.value )
			segs.push_back( { "developer", s.text, g_fonts.inter_medium[ fonts::size::normal ] } );
		if ( has_map )
			segs.push_back( { s_map_name, s.text, g_fonts.inter_medium[ fonts::size::normal ] } );
		if ( wm.show_ping.value )
			segs.push_back( { ping_val, s.text, g_fonts.inter_medium[ fonts::size::normal ] } );
		if ( has_velocity )
			segs.push_back( { vel_val, s.text, g_fonts.inter_medium[ fonts::size::normal ] } );
		if ( wm.show_fps.value )
			segs.push_back( { fps_val, s.text, g_fonts.inter_medium[ fonts::size::normal ] } );
		if ( has_tick )
			segs.push_back( { tick_val, s.text, g_fonts.inter_medium[ fonts::size::normal ] } );
		if ( wm.show_time.value )
			segs.push_back( { time_buf, s.text, g_fonts.inter_medium[ fonts::size::normal ] } );

		const auto sep_str = "|";
		const auto [sep_w, sep_h] = xdraw::measure_text( sep_str, g_fonts.inter_medium[ fonts::size::normal ] );

		float total_w = pad_x * 2.0f;
		for ( std::size_t i = 0; i < segs.size( ); ++i )
		{
			const auto [tw, th] = xdraw::measure_text( segs[ i ].text, segs[ i ].font );
			total_w += tw;
			if ( i + 1 < segs.size( ) )
			{
				total_w += sep_spacing + sep_w + sep_spacing;
			}
		}

		static auto smoothed_w{ 0.0f };
		if ( smoothed_w == 0.0f ) smoothed_w = total_w;
		smoothed_w += ( total_w - smoothed_w ) * std::min( 10.0f * xdraw::delta_time( ), 1.0f );

		const auto w = smoothed_w;
		const auto x = static_cast< float >( screen_w ) - w - margin;
		const auto y = margin;

		// Skeet multi-layer window border
		draw_list.rect( x, y, w, h, xdraw::color{ 0, 0, 0, 255 }, xdraw::corner_radius{ 0.0f }, 1.0f );
		draw_list.rect( x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, xdraw::color{ 48, 48, 48, 255 }, xdraw::corner_radius{ 0.0f }, 1.0f );
		draw_list.rect_filled( x + 2.0f, y + 2.0f, w - 4.0f, h - 4.0f, xdraw::color{ 16, 16, 16, 245 }, xdraw::corner_radius{ 0.0f } );

		// Signature Skeet Top Accent Stripe
		draw_list.rect_filled_gradient( x + 2.0f, y + 2.0f, w - 4.0f, 2.0f, s.accent, tokens::col_accent_purple, tokens::col_accent_purple, s.accent );

		float cur_x = x + pad_x;
		for ( std::size_t i = 0; i < segs.size( ); ++i )
		{
			const auto [tw, th] = xdraw::measure_text( segs[ i ].text, segs[ i ].font );
			const auto ty = std::floor( y + ( h - th ) * 0.5f + 1.0f );
			draw_list.text( std::floor( cur_x ), ty, segs[ i ].text, segs[ i ].color, segs[ i ].font );
			cur_x += tw;

			if ( i + 1 < segs.size( ) )
			{
				cur_x += sep_spacing;
				const auto sty = std::floor( y + ( h - sep_h ) * 0.5f + 1.0f );
				draw_list.text( std::floor( cur_x ), sty, sep_str, xdraw::color{ 70, 70, 70, 255 }, g_fonts.inter_medium[ fonts::size::normal ] );
				cur_x += sep_w + sep_spacing;
			}
		}
	}

	void widgets::keybinds( xdraw::draw_list& draw_list )
	{
		struct row_anim_t
		{
			animation::fade alpha;
			animation::spring offset_y;
			bool active_this_frame{ false };
		};

		static std::map<std::string, row_anim_t> row_states;
		static animation::fade container_alpha;
		static animation::spring smoothed_base_y;

		const auto [screen_w, screen_h] = xdraw::viewport_size( );
		const auto& s = xui::ctx( ).style;

		constexpr auto margin{ 10.0f };
		constexpr auto row_spacing{ 3.0f };
		constexpr auto row_h{ 21.0f };
		constexpr auto header_h{ 24.0f };
		constexpr auto r{ 2.0f };
		constexpr auto inner_pad{ 2.0f };
		constexpr auto text_pad_x{ 8.0f };
		constexpr auto text_nudge{ 0.5f };
		constexpr auto icon_size{ 20.0f };

		struct bind_entry
		{
			const char* name;
			char value[ 32 ];
			bool has_value_pill;
			xui::bind_mode mode;
		};

		bind_entry entries[ 32 ]{};
		auto count{ 0 };

		const auto& ctx = features::combat::g_shared.ctx( );
		const auto has_weapon = ctx.valid && ctx.weapon_type >= cstypes::weapon_type::pistol && ctx.weapon_type <= cstypes::weapon_type::lmg;

		for ( const auto setting : xui::binds::all( ) )
		{
			if ( !setting || setting->bind.key == 0 || !setting->bind.active || count >= 32 )
			{
				continue;
			}

			auto is_rage_group{ false };
			for ( auto i = 0u; i < settings::combat::ragebot::k_group_count; ++i )
			{
				const auto& g = settings::g_combat.m_ragebot.groups[ i ];
				if ( setting == &g.min_damage_override || setting == &g.hitchance_override || setting == &g.force_shot || setting == &g.force_shot_air || setting == &g.body_aim || setting == &g.silent || setting == &g.no_spread )
				{
					is_rage_group = true;
					break;
				}
			}

			if ( is_rage_group )
			{
				continue;
			}

			auto is_legit_group{ false };
			for ( auto i = 0u; i < settings::combat::legitbot::k_group_count; ++i )
			{
				const auto& g = settings::g_combat.m_legitbot.groups[ i ];
				if ( setting == &g.aimbot || setting == &g.rcs || setting == &g.standalone_rcs || setting == &g.triggerbot || setting == &g.autowall || setting == &g.visualize_fov || setting == &g.trigger_head_only || setting == &g.give_me_your_seed )
				{
					is_legit_group = true;
					break;
				}
			}

			if ( is_legit_group )
			{
				if ( !settings::g_combat.m_legitbot.enabled.value || !has_weapon )
				{
					continue;
				}

				const auto* active_group = &settings::g_combat.m_legitbot.get_group( ctx.weapon_type );
				auto is_active{ false };

				for ( auto i = 0u; i < settings::combat::legitbot::k_group_count; ++i )
				{
					if ( &settings::g_combat.m_legitbot.groups[ i ] == active_group )
					{
						const auto& g = settings::g_combat.m_legitbot.groups[ i ];
						if ( setting == &g.aimbot || setting == &g.rcs || setting == &g.standalone_rcs || setting == &g.triggerbot || setting == &g.autowall || setting == &g.visualize_fov || setting == &g.trigger_head_only || setting == &g.give_me_your_seed )
						{
							is_active = true;
						}

						if ( is_active && setting == &active_group->give_me_your_seed && !active_group->triggerbot.value )
						{
							is_active = false;
						}
						break;
					}
				}

				if ( !is_active )
				{
					continue;
				}

				auto& e = entries[ count++ ];
				e.name = setting->name.c_str( );
				e.mode = setting->bind.mode;
				e.value[ 0 ] = '\0';
				e.has_value_pill = false;
				continue;
			}

			if ( setting == &settings::g_combat.m_antiaim.enabled || setting == &settings::g_combat.m_antiaim.manual_left || setting == &settings::g_combat.m_antiaim.manual_right || setting == &settings::g_combat.m_antiaim.hide_shots || setting == &settings::g_combat.m_antiaim.avoid_backstab || setting == &settings::g_combat.m_antiaim.direction_indicator )
			{
				if ( !settings::g_combat.m_antiaim.enabled.value )
				{
					continue;
				}
			}

			auto& e = entries[ count++ ];
			e.name = setting->name.c_str( );
			e.mode = setting->bind.mode;
			e.value[ 0 ] = '\0';
			e.has_value_pill = false;
		}

		if ( count > 0 )
			container_alpha.fade_in( 0.2f );
		else
			container_alpha.fade_out( 0.2f );

		container_alpha.update( );
		if ( !container_alpha.visible( ) )
			return;

		const auto master_alpha = container_alpha.alpha( );
		const auto total_h = header_h + row_spacing + ( static_cast< float >( count ) * ( row_h + row_spacing ) );
		const auto target_base_y = ( static_cast< float >( screen_h ) * 0.5f ) - ( total_h * 0.5f );

		smoothed_base_y.set_target( target_base_y );
		smoothed_base_y.update( );

		const auto base_ry = smoothed_base_y.value( );
		const auto x = margin;

		static auto icon_w_px = 0, icon_h_px = 0;
		static const auto kb_icon = xdraw::load_svg( R"(<svg width="12" height="12" viewBox="0 0 12 12" fill="none" xmlns="http://www.w3.org/2000/svg"><path d="M2.78571 4.07143C2.53142 4.07143 2.28285 3.99602 2.07141 3.85475C1.85998 3.71347 1.69518 3.51267 1.59787 3.27774C1.50056 3.0428 1.4751 2.78429 1.52471 2.53488C1.57431 2.28548 1.69677 2.05639 1.87658 1.87658C2.05639 1.69677 2.28548 1.57431 2.53488 1.52471C2.78429 1.4751 3.0428 1.50056 3.27774 1.59787C3.51267 1.69518 3.71347 1.85998 3.85475 2.07141C3.99602 2.28285 4.07143 2.53142 4.07143 2.78571V9.21429C4.07143 9.46858 3.99602 9.71716 3.85475 9.92859C3.71347 10.14 3.51267 10.3048 3.27774 10.4021C3.0428 10.4994 2.78429 10.5249 2.53488 10.4753C2.28548 10.4257 2.05639 10.3032 1.87658 10.1234C1.69677 9.94361 1.57431 9.71452 1.52471 9.46512C1.4751 9.21571 1.50056 8.9572 1.59787 8.72226C1.69518 8.48733 1.85998 8.28653 2.07141 8.14525C2.28285 8.00398 2.53142 7.92857 2.78571 7.92857H9.21429C9.46858 7.92857 9.71716 8.00398 9.92859 8.14525C10.14 8.28653 10.3048 8.48733 10.4021 8.72226C10.4994 8.9572 10.5249 9.21571 10.4753 9.46512C10.4257 9.71452 10.3032 9.94361 10.1234 10.1234C9.94361 10.3032 9.71452 10.4257 9.46512 10.4753C9.21571 10.5249 8.9572 10.4994 8.72226 10.4021C8.48733 10.3048 8.28653 10.14 8.14525 9.92859C8.00398 9.71716 7.92857 9.46858 7.92857 9.21429V2.78571C7.92857 2.53142 8.00398 2.28285 8.14525 2.07141C8.28653 1.85998 8.48733 1.69518 8.72226 1.59787C8.9572 1.50056 9.21571 1.4751 9.46512 1.52471C9.71452 1.57431 9.94361 1.69677 10.1234 1.87658C10.3032 2.05639 10.4257 2.28548 10.4753 2.53488C10.5249 2.78429 10.4994 3.0428 10.4021 3.27774C10.3048 3.51267 10.14 3.71347 9.92859 3.85475C9.71716 3.99602 9.46858 4.07143 9.21429 4.07143H2.78571Z" stroke="#FFFFFF" stroke-linecap="round" stroke-linejoin="round"/></svg>)", 1.0f, &icon_w_px, &icon_h_px );

		const auto [header_tw, header_th] = xdraw::measure_text( "keybinds" );
		const auto header_w = inner_pad + icon_size + inner_pad + header_tw + text_pad_x * 2.0f + inner_pad;
		const auto master_u8 = static_cast< std::uint8_t >( 255.0f * master_alpha );

		draw_list.rect_filled_blurred( x, base_ry, header_w, header_h, xdraw::corner_radius{ r }, xdraw::color{ 255, 255, 255, master_u8 } );
		// Skeet multi-layer header frame
		draw_list.rect( x, base_ry, header_w, header_h, xdraw::color{ 0, 0, 0, master_u8 }, xdraw::corner_radius{ 0.0f }, 1.0f );
		draw_list.rect( x + 1.0f, base_ry + 1.0f, header_w - 2.0f, header_h - 2.0f, xdraw::color{ 48, 48, 48, master_u8 }, xdraw::corner_radius{ 0.0f }, 1.0f );
		draw_list.rect_filled( x + 2.0f, base_ry + 2.0f, header_w - 4.0f, header_h - 4.0f, xdraw::color{ 16, 16, 16, static_cast< std::uint8_t >( 245.0f * master_alpha ) }, xdraw::corner_radius{ 0.0f } );
		draw_list.rect_filled_gradient( x + 2.0f, base_ry + 2.0f, header_w - 4.0f, 1.5f, s.accent.alpha( master_u8 ), tokens::col_accent_purple.alpha( master_u8 ), tokens::col_accent_purple.alpha( master_u8 ), s.accent.alpha( master_u8 ) );

		const auto htx = x + 8.0f;
		draw_list.text( htx, base_ry + ( header_h - header_th ) * 0.5f + text_nudge, "keybinds", s.accent.alpha( master_u8 ) );

		for ( auto& [name, state] : row_states )
			state.active_this_frame = false;

		float current_offset_y = header_h + row_spacing;
		for ( auto i = 0; i < count; ++i )
		{
			const auto& e = entries[ i ];
			auto& anim = row_states[ e.name ];

			if ( !anim.active_this_frame && anim.alpha.alpha( ) <= 0.01f )
				anim.offset_y.snap( current_offset_y );

			anim.active_this_frame = true;
			anim.alpha.fade_in( 0.2f );
			anim.offset_y.set_target( current_offset_y );
			anim.alpha.update( );
			anim.offset_y.update( );

			const auto row_alpha = anim.alpha.alpha( ) * master_alpha;
			const auto draw_y = base_ry + anim.offset_y.value( );
			const auto [nw, nh] = xdraw::measure_text( e.name );
			const auto row_u8 = static_cast< std::uint8_t >( 255.0f * row_alpha );

			if ( e.has_value_pill )
			{
				const auto [vw, vh] = xdraw::measure_text( e.value );
				const auto name_pill_w = nw + text_pad_x * 2.0f;
				const auto value_pill_w = vw + text_pad_x * 2.0f;
				const auto row_w = name_pill_w + value_pill_w;

				draw_list.rect( x, draw_y, row_w, row_h, xdraw::color{ 0, 0, 0, row_u8 }, xdraw::corner_radius{ 0.0f }, 1.0f );
				draw_list.rect( x + 1.0f, draw_y + 1.0f, row_w - 2.0f, row_h - 2.0f, xdraw::color{ 44, 44, 44, row_u8 }, xdraw::corner_radius{ 0.0f }, 1.0f );
				draw_list.rect_filled( x + 2.0f, draw_y + 2.0f, row_w - 4.0f, row_h - 4.0f, xdraw::color{ 18, 18, 18, static_cast< std::uint8_t >( 245.0f * row_alpha ) }, xdraw::corner_radius{ 0.0f } );
				draw_list.text( x + text_pad_x, draw_y + ( row_h - nh ) * 0.5f + text_nudge, e.name, s.accent.alpha( row_u8 ) );
				draw_list.text( x + name_pill_w + text_pad_x, draw_y + ( row_h - vh ) * 0.5f + text_nudge, e.value, s.text.alpha( row_u8 ) );
			}
			else
			{
				const auto name_pill_w = nw + text_pad_x * 2.0f;
				const auto row_w = name_pill_w;
				const auto text_col = ( e.mode == xui::bind_mode::toggle ) ? s.text_dim : s.accent;

				draw_list.rect( x, draw_y, row_w, row_h, xdraw::color{ 0, 0, 0, row_u8 }, xdraw::corner_radius{ 0.0f }, 1.0f );
				draw_list.rect( x + 1.0f, draw_y + 1.0f, row_w - 2.0f, row_h - 2.0f, xdraw::color{ 44, 44, 44, row_u8 }, xdraw::corner_radius{ 0.0f }, 1.0f );
				draw_list.rect_filled( x + 2.0f, draw_y + 2.0f, row_w - 4.0f, row_h - 4.0f, xdraw::color{ 18, 18, 18, static_cast< std::uint8_t >( 245.0f * row_alpha ) }, xdraw::corner_radius{ 0.0f } );
				draw_list.text( x + text_pad_x, draw_y + ( row_h - nh ) * 0.5f + text_nudge, e.name, text_col.alpha( row_u8 ) );
			}

			current_offset_y += row_h + row_spacing;
		}

		for ( auto it = row_states.begin( ); it != row_states.end( ); )
		{
			if ( !it->second.active_this_frame )
			{
				it->second.alpha.fade_out( 0.15f );
				it->second.alpha.update( );
				it->second.offset_y.update( );

				if ( it->second.alpha.alpha( ) <= 0.001f )
				{
					it = row_states.erase( it );
					continue;
				}

				const auto row_alpha = it->second.alpha.alpha( ) * master_alpha;
				const auto row_u8 = static_cast< std::uint8_t >( 255.0f * row_alpha );
				const auto draw_y = base_ry + it->second.offset_y.value( );
				const auto [nw, nh] = xdraw::measure_text( it->first.c_str( ) );
				const auto row_w = nw + text_pad_x * 2.0f;

				draw_list.rect( x, draw_y, row_w, row_h, xdraw::color{ 0, 0, 0, row_u8 }, xdraw::corner_radius{ 0.0f }, 1.0f );
				draw_list.rect_filled( x + 2.0f, draw_y + 2.0f, row_w - 4.0f, row_h - 4.0f, xdraw::color{ 18, 18, 18, static_cast< std::uint8_t >( 245.0f * row_alpha ) }, xdraw::corner_radius{ 0.0f } );
			}
			++it;
		}
	}

} // namespace rendering
