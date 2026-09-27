#include <pch/pch.hpp>
#include <core/settings.hpp>
#include <core/features/features.hpp>

#include "../../rendering.hpp"

namespace rendering {

	void menu::draw_world( float group_w ) const
	{
		auto& w = settings::g_world;
		auto& esp = settings::g_esp;
		auto& m = settings::g_misc;

		const auto wx = this->m_x;
		const auto wy = this->m_y;
		const auto content_x = this->m_body_x;
		const auto body_y = this->m_body_y;
		const auto content_w = this->m_body_w;
		const auto col_w = ( content_w - tokens::gap ) * 0.5f;
		const auto right_x = content_x + col_w + tokens::gap;

		// ==========================================
		// LEFT COLUMN: Items & Objectives ESP
		// ==========================================
		xui::layout::set_cursor( content_x - wx, body_y - wy );

		if ( xui::begin_child( "Items & Objectives##vis_items", col_w, this->m_body_h ) )
		{
			// Dropped items
			xui::checkbox( "Dropped weapons & items", esp.m_item.m_overlay.enabled );
			if ( xui::begin_popup( "##item_settings_popup", 220.0f ) )
			{
				constexpr const char* disp_modes[ ]{ "Text", "Icon", "Text + Icon" };
				xui::combo( "Display##it", esp.m_item.m_overlay.groups[ 0 ].display.value, disp_modes, 3 );
				xui::slider_float( "Max distance##it", esp.m_item.m_overlay.groups[ 0 ].max_distance, 1.0f, 200.0f, "%.0fm" );
				xui::color_picker( "Text color##it", esp.m_item.m_overlay.groups[ 0 ].text_color );
				xui::color_picker( "Icon color##it", esp.m_item.m_overlay.groups[ 0 ].icon_color );
				xui::end_popup( );
			}

			// Grenade trajectory
			xui::checkbox( "Grenade trajectory", m.m_projectile_trajectory.enabled );
			if ( xui::begin_popup( "##proj_settings_popup", 220.0f ) )
			{
				xui::checkbox( "Straight throw", m.m_projectile_trajectory.straight_throw );
				xui::checkbox( "Glow effect", m.m_projectile_trajectory.glow );
				xui::color_picker( "Held color", m.m_projectile_trajectory.held_color );
				xui::color_picker( "Thrown color", m.m_projectile_trajectory.thrown_color );
				xui::end_popup( );
			}

			// Grenade Timers & Tracers
			xui::checkbox( "Grenade timers", esp.m_other.grenade_timer );
			xui::checkbox( "Grenade tracers", esp.m_other.grenade_tracers );

			// Bomb / C4 Timer
			xui::checkbox( "Bomb / C4 timer", esp.m_other.bomb_timer );
			xui::checkbox( "Spectator list", esp.m_other.spectator_list );

			xui::end_child( );
		}

		// ==========================================
		// RIGHT COLUMN: World Environment & Effects
		// ==========================================
		xui::layout::set_cursor( right_x - wx, body_y - wy );

		if ( xui::begin_child( "World & Environment##vis_env", col_w, this->m_body_h ) )
		{
			// Weather effects
			xui::checkbox( "Weather effects", w.m_weather.enabled );
			if ( xui::begin_popup( "##weather_settings_popup", 220.0f ) )
			{
				constexpr const char* weather_types[ ]{ "Snow", "Rain", "Stars" };
				xui::combo( "Type##wthr", w.m_weather.type.value, weather_types, 3 );
				xui::checkbox( "Fog effect", w.m_weather.fog_enabled );
				if ( w.m_weather.fog_enabled )
				{
					xui::slider_float( "Fog density", w.m_weather.fog_density, 0.05f, 2.0f, "%.2f" );
					xui::color_picker( "Fog color", w.m_weather.fog_color );
				}
				xui::color_picker( "Particle color", w.m_weather.color );
				xui::end_popup( );
			}

			// Ambient lighting / Nightmode
			xui::checkbox( "World ambient lighting", w.m_scene.ambient );
			if ( xui::begin_popup( "##ambient_popup", 220.0f ) )
			{
				xui::slider_float( "Intensity##amb", w.m_scene.ambient_intensity, 0.1f, 3.0f, "%.2f" );
				xui::color_picker( "Color##amb", w.m_scene.ambient_color );
				xui::end_popup( );
			}

			// Skybox changer
			xui::checkbox( "Skybox changer", w.m_scene.skybox.custom_skybox );
			if ( xui::begin_popup( "##skybox_popup", 220.0f ) )
			{
				const auto& skyboxes = features::world::g_scene.get_skyboxes( );
				if ( !skyboxes.empty( ) )
				{
					std::vector<const char*> names;
					names.reserve( skyboxes.size( ) );
					for ( const auto& skybox : skyboxes )
					{
						names.push_back( skybox.display_name.c_str( ) );
					}

					w.m_scene.skybox.selected_skybox.value = std::clamp(
						w.m_scene.skybox.selected_skybox.value, 0,
						static_cast< int >( skyboxes.size( ) ) - 1 );
					xui::combo(
						"Skybox##sky", w.m_scene.skybox.selected_skybox.value,
						names.data( ), static_cast< int >( names.size( ) ) );
				}
				xui::checkbox( "Custom tint color", w.m_scene.skybox.custom_color );
				if ( w.m_scene.skybox.custom_color )
				{
					xui::color_picker( "Sky color", w.m_scene.skybox.skybox_color );
					xui::color_picker( "Cloud color", w.m_scene.skybox.cloud_color );
					xui::color_picker( "Sun color", w.m_scene.skybox.sun_color );
				}
				xui::end_popup( );
			}

			// Bullet Tracers
			xui::checkbox( "Bullet tracers", m.m_impacts.bullet_tracers );
			if ( xui::begin_popup( "##bullet_tracers_popup", 220.0f ) )
			{
				xui::slider_float( "Duration##trc", m.m_impacts.bullet_tracer_duration, 0.1f, 5.0f, "%.1fs" );
				xui::color_picker( "Color##trc", m.m_impacts.bullet_tracer_color );
				xui::end_popup( );
			}

			xui::end_child( );
		}
	}

} // namespace rendering
