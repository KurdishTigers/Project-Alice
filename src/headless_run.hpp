#pragma once
// -headless -years N: runs N game years with no speed cap, prints seconds per game year, and with -dump DIR
// writes monthly CSVs (nations.csv, prices.csv, provinces.csv) on the 1st of each month.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <oneapi/tbb/task_arena.h>

namespace headless {

inline std::string ymd_str(sys::year_month_day d) {
	char buf[16];
	snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.year, int(d.month), int(d.day));
	return std::string(buf);
}
inline std::string ymd_of(sys::state& state) {
	return ymd_str(state.current_date.to_ymd(state.start_date));
}

inline std::string tag_of(sys::state& state, dcon::nation_id n) {
	if(!n)
		return "";
	return nations::int_to_tag(
			state.world.national_identity_get_identifying_int(state.world.nation_get_identity_from_identity_holder(n)));
}

inline void dump_month(sys::state& state, FILE* nations_csv, FILE* prices_csv, FILE* provinces_csv, std::string const& date) {
	// province index is Alice's own (land provinces first), not the Vic2 definition.csv number
	for(int32_t i = 0; i < state.province_definitions.first_sea_province.index(); ++i) {
		dcon::province_id p{dcon::province_id::value_base_t(i)};
		fprintf(provinces_csv, "%s,%d,%s,%s,%.6g\n", date.c_str(), i,
				tag_of(state, state.world.province_get_nation_from_province_ownership(p)).c_str(),
				tag_of(state, state.world.province_get_nation_from_province_control(p)).c_str(),
				state.world.province_get_demographics(p, demographics::total));
	}
	state.world.for_each_nation([&](dcon::nation_id n) {
		if(state.world.nation_get_owned_province_count(n) == 0)
			return;
		fprintf(nations_csv, "%s,%s,%.6g,%.6g,%.6g,%d,%d,%d,%.6g,%.6g,%d,%.6g,%.6g\n", date.c_str(), tag_of(state, n).c_str(),
				state.world.nation_get_prestige(n), float(state.world.nation_get_industrial_score(n)),
				float(state.world.nation_get_military_score(n)), int(state.world.nation_get_rank(n)),
				int(state.world.nation_get_is_great_power(n)), int(state.world.nation_get_is_civilized(n)),
				state.world.nation_get_treasury(n), state.world.nation_get_demographics(n, demographics::total),
				int(state.world.nation_get_owned_province_count(n)), state.world.nation_get_infamy(n),
				state.world.nation_get_war_exhaustion(n));
	});
	state.world.for_each_commodity([&](dcon::commodity_id c) {
		if(c == economy::money)
			return;
		float supply = 0.f;
		state.world.for_each_market([&](dcon::market_id m) { supply += state.world.market_get_supply(m, c); });
		// price = supply-weighted mean over the per-state markets; median_price = median over markets
		fprintf(prices_csv, "%s,%s,%.6g,%.6g,%.6g\n", date.c_str(),
				text::produce_simple_string(state, state.world.commodity_get_name(c)).c_str(), economy::price(state, c),
				economy::median_price(state, c), supply);
	});
}

// checkpoint_every > 0: also writes a save named CKPT_<year> every that many years (for starting twin runs late in the game)
inline void run(sys::state& state, int years, std::string const& dump_dir, int checkpoint_every = 0) {
	using clock = std::chrono::steady_clock;
	FILE* nations_csv = nullptr;
	FILE* prices_csv = nullptr;
	FILE* provinces_csv = nullptr;
	if(!dump_dir.empty()) {
		std::filesystem::create_directories(dump_dir);
		nations_csv = fopen((dump_dir + "/nations.csv").c_str(), "w");
		prices_csv = fopen((dump_dir + "/prices.csv").c_str(), "w");
		provinces_csv = fopen((dump_dir + "/provinces.csv").c_str(), "w");
		fprintf(provinces_csv, "date,province,owner,controller,population\n");
		fprintf(nations_csv, "date,tag,prestige,industrial_score,military_score,rank,is_gp,is_civilized,treasury,population,provinces,infamy,war_exhaustion\n");
		fprintf(prices_csv, "date,good,price,median_price,world_supply\n");
	}

	auto start = state.current_date.to_ymd(state.start_date);
	int const end_year = start.year + years;
	int year = start.year;
	printf("HEADLESS start=%s years=%d seed=%u pops=%u\n", ymd_str(start).c_str(), years, state.game_seed, state.world.pop_size());
	fflush(stdout);
	if(nations_csv)
		dump_month(state, nations_csv, prices_csv, provinces_csv, ymd_str(start));

	state.user_settings.autosaves = sys::autosave_frequency::none; // parallel runs would share one autosave slot set
	auto t_start = clock::now();
	auto t_year = t_start;
	while(years > 0) {
		command::execute_pending_commands(state);
		state.single_game_tick();
		auto ymd = state.current_date.to_ymd(state.start_date);
		if(ymd.day == 1 && nations_csv) {
			dump_month(state, nations_csv, prices_csv, provinces_csv, ymd_str(ymd));
			fflush(nations_csv);
			fflush(prices_csv);
			fflush(provinces_csv);
		}
		if(ymd.year != year) {
			auto now = clock::now();
			printf("YEAR %d secs=%.3f pops=%u wars=%u\n", year, std::chrono::duration<double>(now - t_year).count(),
					state.world.pop_size(), state.world.war_size());
			fflush(stdout);
			t_year = now;
			year = ymd.year;
			if(checkpoint_every > 0 && (year - start.year) % checkpoint_every == 0) {
				auto name = "CKPT_" + std::to_string(year);
				sys::write_save_file(state, sys::save_type::normal, name, name);
				printf("CHECKPOINT %s\n", name.c_str());
				fflush(stdout);
			}
		}
		if(ymd.year >= end_year)
			break;
	}
	printf("TOTAL years=%d secs=%.3f\n", years, std::chrono::duration<double>(clock::now() - t_start).count());
	fflush(stdout);
	if(nations_csv) {
		fclose(nations_csv);
		fclose(prices_csv);
		fclose(provinces_csv);
	}
}

// -twin: runs two copies of the same game in one process, side by side, and compares the full multiplayer
// checksum every `check_every` days. The second copy runs inside a task arena capped at `b_threads` worker
// threads, so it behaves like a client on a PC with a different core count. On the first mismatch it writes
// a field-by-field OOS report plus a save of each copy to `out_dir`, then stops.
// Returns the number of days simulated before the split, or -1 if both copies stayed identical.
// roundtrip_every > 0: every that many days, copy A is saved to memory and loaded into copy B the same way a client
// loads the host's save when it joins or resyncs (clear unsaved data, read save, fill unsaved data), and the two are
// compared straight away. A mismatch there means some synced state does not survive a save/load, which breaks
// hot-joining and makes resyncs fail.
inline int run_twin(sys::state& a, sys::state& b, int years, int b_threads, int check_every, std::string const& out_dir, int roundtrip_every = 0) {
	using clock = std::chrono::steady_clock;
	std::filesystem::create_directories(out_dir);
	a.user_settings.autosaves = sys::autosave_frequency::none;
	b.user_settings.autosaves = sys::autosave_frequency::none;

	oneapi::tbb::task_arena arena_b(b_threads > 0 ? b_threads : oneapi::tbb::task_arena::automatic);

	auto start = a.current_date.to_ymd(a.start_date);
	int const end_year = start.year + years;
	int year = start.year;
	printf("TWIN start=%s years=%d seed=%u/%u b_threads=%d check_every=%d\n", ymd_str(start).c_str(), years, a.game_seed,
			b.game_seed, b_threads, check_every);
	fflush(stdout);

	auto compare = [&](int day) {
		auto ka = a.get_mp_state_checksum();
		auto kb = b.get_mp_state_checksum();
		if(ka.is_equal(kb))
			return true;
		auto date = ymd_of(a);
		printf("SPLIT day=%d date=%s\n", day, date.c_str());
		fflush(stdout);
		std::string report = network::generate_full_oos_report(a, b);
		FILE* f = fopen((out_dir + "/oos_report_" + date + ".txt").c_str(), "w");
		if(f) {
			fwrite(report.data(), 1, report.size(), f);
			fclose(f);
		}
		sys::write_save_file(a, sys::save_type::normal, "TWIN_A_" + date, "TWIN_A_" + date);
		sys::write_save_file(b, sys::save_type::normal, "TWIN_B_" + date, "TWIN_B_" + date);
		return false;
	};

	auto roundtrip = [&](int day) {
		auto length = sys::sizeof_save_section(a);
		auto buffer = std::unique_ptr<uint8_t[]>(new uint8_t[length]);
		sys::write_save_section(buffer.get(), a);
		b.clear_unsaved_data();
		sys::read_save_section(buffer.get(), buffer.get() + length, b);
		b.fill_unsaved_data();
		if(!compare(day)) {
			printf("ROUNDTRIP_SPLIT day=%d (state did not survive save + load)\n", day);
			fflush(stdout);
			return false;
		}
		return true;
	};

	if(!compare(0))
		return 0;
	if(roundtrip_every > 0 && !roundtrip(0))
		return 0;

	auto t_start = clock::now();
	auto t_year = t_start;
	int day = 0;
	while(true) {
		a.single_game_tick();
		arena_b.execute([&]() { b.single_game_tick(); });
		++day;
		if(day % check_every == 0 && !compare(day))
			return day;
		if(roundtrip_every > 0 && day % roundtrip_every == 0 && !roundtrip(day))
			return day;
		auto ymd = a.current_date.to_ymd(a.start_date);
		if(ymd.year != year) {
			auto now = clock::now();
			printf("YEAR %d ok secs=%.1f wars=%u\n", year, std::chrono::duration<double>(now - t_year).count(), a.world.war_size());
			fflush(stdout);
			t_year = now;
			year = ymd.year;
		}
		if(ymd.year >= end_year)
			break;
	}
	printf("TWIN done, no split in %d days (%.0f secs)\n", day, std::chrono::duration<double>(clock::now() - t_start).count());
	fflush(stdout);
	return -1;
}

} // namespace headless
