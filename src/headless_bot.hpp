#pragma once
// Test-only "player bot" for headless multiplayer runs (-bot). Once per game day it plays the local nation the way a
// busy human would, but only through the real networked commands the UI uses, so every order goes client -> host ->
// all players. The point is to exercise player-command code paths that AI-only runs never touch, while the game
// checks for desyncs every day. It does not try to play well.
#include <random>
#include <vector>

namespace headless_bot {

struct bot {
	std::mt19937 rng{12345};
	int32_t last_day = -1;
	int32_t orders_sent = 0;

	template<typename T>
	T pick(std::vector<T> const& v) {
		return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)];
	}
	bool chance(int one_in) {
		return std::uniform_int_distribution<int>(1, one_in)(rng) == 1;
	}

	void step(sys::state& state) {
		auto n = state.local_player_nation;
		if(!n || !state.current_scene.game_in_progress || state.network_state.out_of_sync)
			return;
		if(state.current_date.value == last_day)
			return;
		last_day = state.current_date.value;

		answer_events(state, n);
		keep_researching(state, n);
		if(chance(30))
			shuffle_budget(state, n);
		if(chance(3))
			move_an_army(state, n);
		if(chance(15))
			recruit(state, n);
		if(chance(25))
			build_factory(state, n);
		if(chance(25))
			build_railroad(state, n);
		if(chance(10))
			take_a_decision(state, n);
	}

	void answer_events(sys::state& state, dcon::nation_id n) {
		for(auto& e : state.pending_n_event)
			if(e.n == n) { command::make_event_choice(state, e, 0); ++orders_sent; }
		for(auto& e : state.pending_f_n_event)
			if(e.n == n) { command::make_event_choice(state, e, 0); ++orders_sent; }
		for(auto& e : state.pending_p_event)
			if(state.world.province_get_nation_from_province_ownership(e.p) == n) { command::make_event_choice(state, e, 0); ++orders_sent; }
		for(auto& e : state.pending_f_p_event)
			if(state.world.province_get_nation_from_province_ownership(e.p) == n) { command::make_event_choice(state, e, 0); ++orders_sent; }
	}

	void keep_researching(sys::state& state, dcon::nation_id n) {
		if(state.world.nation_get_current_research(n))
			return;
		std::vector<dcon::technology_id> options;
		for(auto t : state.world.in_technology)
			if(command::can_start_research(state, n, t))
				options.push_back(t);
		if(!options.empty()) { command::start_research(state, n, pick(options)); ++orders_sent; }
	}

	void shuffle_budget(sys::state& state, dcon::nation_id n) {
		auto r = [&](int lo, int hi) { return int8_t(std::uniform_int_distribution<int>(lo, hi)(rng)); };
		command::budget_settings_data b{};
		b.education_spending = r(30, 100);
		b.military_spending = r(30, 100);
		b.administrative_spending = r(30, 100);
		b.social_spending = r(0, 100);
		b.land_spending = r(30, 100);
		b.naval_spending = r(30, 100);
		b.construction_spending = r(30, 100);
		b.stockpile_spending = r(0, 50);
		b.poor_tax = r(10, 80);
		b.middle_tax = r(10, 80);
		b.rich_tax = r(10, 80);
		b.tariffs_import = r(0, 30);
		b.tariffs_export = r(0, 30);
		b.domestic_investment = r(0, 50);
		b.overseas = r(0, 50);
		b.subsidies = 0;
		command::change_budget_settings(state, n, b);
		++orders_sent;
	}

	void move_an_army(sys::state& state, dcon::nation_id n) {
		std::vector<dcon::army_id> armies;
		for(auto ac : state.world.nation_get_army_control(n)) {
			auto a = ac.get_army();
			if(!a.get_battle_from_army_battle_participation() && !a.get_navy_from_army_transport())
				armies.push_back(a.id);
		}
		if(armies.empty())
			return;
		auto a = pick(armies);
		auto loc = state.world.army_get_location_from_army_location(a);
		std::vector<dcon::province_id> targets;
		for(auto adj : state.world.province_get_province_adjacency(loc)) {
			auto other = adj.get_connected_provinces(0) == loc ? adj.get_connected_provinces(1) : adj.get_connected_provinces(0);
			if(other && other.id.index() < state.province_definitions.first_sea_province.index())
				targets.push_back(other.id);
		}
		if(targets.empty())
			return;
		auto dest = pick(targets);
		if(!command::can_move_army(state, n, a, dest).empty()) { command::move_army(state, n, a, dest, true); ++orders_sent; }
	}

	void recruit(sys::state& state, dcon::nation_id n) {
		std::vector<dcon::province_id> provs;
		for(auto po : state.world.nation_get_province_ownership(n))
			provs.push_back(po.get_province().id);
		if(provs.empty())
			return;
		auto p = pick(provs);
		auto culture = state.world.nation_get_primary_culture(n);
		std::vector<dcon::unit_type_id> types;
		for(uint32_t i = 0; i < state.military_definitions.unit_base_definitions.size(); ++i) {
			dcon::unit_type_id t{ dcon::unit_type_id::value_base_t(i) };
			if(state.military_definitions.unit_base_definitions[t].is_land)
				types.push_back(t);
		}
		if(types.empty())
			return;
		auto t = pick(types);
		if(command::can_start_land_unit_construction<false>(state, n, p, culture, t, dcon::province_id{})) { command::start_land_unit_construction(state, n, p, culture, t); ++orders_sent; }
	}

	void build_factory(sys::state& state, dcon::nation_id n) {
		std::vector<dcon::province_id> provs;
		for(auto po : state.world.nation_get_province_ownership(n))
			provs.push_back(po.get_province().id);
		if(provs.empty() || state.world.factory_type_size() == 0)
			return;
		for(int tries = 0; tries < 8; ++tries) {
			auto p = pick(provs);
			dcon::factory_type_id ft{ dcon::factory_type_id::value_base_t(std::uniform_int_distribution<uint32_t>(0, state.world.factory_type_size() - 1)(rng)) };
			if(command::can_begin_factory_building_construction(state, n, p, ft, false)) {
				command::begin_factory_building_construction(state, n, p, ft, false);
				++orders_sent;
				return;
			}
		}
	}

	void build_railroad(sys::state& state, dcon::nation_id n) {
		std::vector<dcon::province_id> provs;
		for(auto po : state.world.nation_get_province_ownership(n))
			provs.push_back(po.get_province().id);
		if(provs.empty())
			return;
		auto p = pick(provs);
		if(command::can_begin_province_building_construction(state, n, p, economy::province_building_type::railroad)) {
			command::begin_province_building_construction(state, n, p, economy::province_building_type::railroad);
			++orders_sent;
		}
	}

	void take_a_decision(sys::state& state, dcon::nation_id n) {
		std::vector<dcon::decision_id> options;
		for(auto d : state.world.in_decision)
			if(command::can_take_decision(state, n, d))
				options.push_back(d);
		if(!options.empty()) { command::take_decision(state, n, pick(options)); ++orders_sent; }
	}
};

} // namespace headless_bot
