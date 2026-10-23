#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include "system_state.hpp"
#include "game_scene.hpp"
#include "serialization.hpp"
#include "parsers_declarations.hpp"

#include <Windows.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <DbgHelp.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "Objbase.h"
#include "window.hpp"

#include "network/webapi/controllers.hpp"

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "icu.lib")
#pragma comment(lib, "Dbghelp.lib")

static sys::state game_state; // too big for the stack
static CRITICAL_SECTION guard_abort_handler;

void signal_abort_handler(int) {
	static bool run_once = false;

	EnterCriticalSection(&guard_abort_handler);
	if(run_once == false) {
		run_once = true;

		STARTUPINFO si;
		ZeroMemory(&si, sizeof(si));
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi;
		ZeroMemory(&pi, sizeof(pi));
		// Start the child process.
		if(CreateProcessW(
			L"dbg_alice.exe",   // Module name
			NULL, // Command line
			NULL, // Process handle not inheritable
			NULL, // Thread handle not inheritable
			FALSE, // Set handle inheritance to FALSE
			0, // No creation flags
			NULL, // Use parent's environment block
			NULL, // Use parent's starting directory 
			&si, // Pointer to STARTUPINFO structure
			&pi) == 0) {

			// create process failed
			LeaveCriticalSection(&guard_abort_handler);
			return;

		}
		// Wait until child process exits.
		WaitForSingleObject(pi.hProcess, INFINITE);
		// Close process and thread handles. 
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
	}
	LeaveCriticalSection(&guard_abort_handler);
}

// Crash reporter: on an unhandled exception, walk the stack with DbgHelp (using Alice.pdb next to the exe when it is
// there), save the report to Documents\Project Alice\crash_report.txt and show it in a message box so a player can
// simply screenshot it.
static void write_crash_report(struct _EXCEPTION_POINTERS* ep) {
	static char report[12000];
	size_t len = 0;
	auto add_line = [&](char const* text) {
		size_t n = strnlen(text, sizeof(report) - len - 1);
		std::memcpy(report + len, text, n);
		len += n;
		report[len] = 0;
	};
	char line[600];

	HANDLE proc = GetCurrentProcess();
	HANDLE thread = GetCurrentThread();
	auto exe_base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));

	wchar_t exe_dir[MAX_PATH] = { 0 };
	GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
	if(auto last_slash = wcsrchr(exe_dir, L'\\'); last_slash)
		*last_slash = 0;
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
	BOOL have_symbols = SymInitializeW(proc, exe_dir, TRUE);

	auto code = ep && ep->ExceptionRecord ? uint32_t(ep->ExceptionRecord->ExceptionCode) : 0u;
	auto address = ep && ep->ExceptionRecord ? reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress) : 0ull;
	snprintf(line, sizeof(line), "Exception 0x%08X at exe+0x%llX (thread %lu)\n\n", code, (unsigned long long)(address - exe_base), (unsigned long)GetCurrentThreadId());
	add_line(line);

	if(ep && ep->ContextRecord) {
		CONTEXT ctx = *ep->ContextRecord;
		STACKFRAME64 frame;
		std::memset(&frame, 0, sizeof(frame));
		frame.AddrPC.Offset = ctx.Rip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = ctx.Rbp;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Offset = ctx.Rsp;
		frame.AddrStack.Mode = AddrModeFlat;

		static DWORD64 symbol_buffer[(sizeof(SYMBOL_INFO) + 512) / sizeof(DWORD64) + 1];
		for(int i = 0; i < 24; ++i) {
			if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
				break;
			uint64_t pc = frame.AddrPC.Offset;
			if(pc == 0)
				break;
			char const* fn_name = "?";
			auto symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
			std::memset(symbol_buffer, 0, sizeof(symbol_buffer));
			symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
			symbol->MaxNameLen = 511;
			DWORD64 displacement = 0;
			if(have_symbols && SymFromAddr(proc, pc, &displacement, symbol))
				fn_name = symbol->Name;
			IMAGEHLP_LINE64 src_line;
			std::memset(&src_line, 0, sizeof(src_line));
			src_line.SizeOfStruct = sizeof(src_line);
			DWORD line_displacement = 0;
			char const* file_name = nullptr;
			if(have_symbols && SymGetLineFromAddr64(proc, pc, &line_displacement, &src_line) && src_line.FileName) {
				file_name = src_line.FileName;
				if(auto src = strstr(file_name, "src\\"); src)
					file_name = src;
			}
			if(file_name)
				snprintf(line, sizeof(line), "%2d  %s  (%s:%lu)\n", i, fn_name, file_name, (unsigned long)src_line.LineNumber);
			else
				snprintf(line, sizeof(line), "%2d  %s  (+0x%llX)\n", i, fn_name, (unsigned long long)(pc - exe_base));
			add_line(line);
		}
	}
	if(have_symbols)
		SymCleanup(proc);

	wchar_t* docs = nullptr;
	if(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs) == S_OK) {
		std::wstring path = std::wstring(docs) + L"\\Project Alice";
		CreateDirectoryW(path.c_str(), nullptr);
		path += L"\\crash_report.txt";
		HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(f != INVALID_HANDLE_VALUE) {
			DWORD written = 0;
			WriteFile(f, report, DWORD(len), &written, nullptr);
			CloseHandle(f);
		}
	}
	CoTaskMemFree(docs);

	MessageBoxA(nullptr, report, "Project Alice crashed - please screenshot this", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

LONG WINAPI uef_wrapper(struct _EXCEPTION_POINTERS* lpTopLevelExceptionFilter) {
	static LONG reported = 0;
	if(InterlockedExchange(&reported, 1) == 0)
		write_crash_report(lpTopLevelExceptionFilter);
	signal_abort_handler(0);
	return EXCEPTION_CONTINUE_SEARCH;
}

void generic_wrapper() {
	signal_abort_handler(0);
}
void invalid_parameter_wrapper(
   const wchar_t* expression,
   const wchar_t* function,
   const wchar_t* file,
   unsigned int line,
   uintptr_t pReserved
) {
	signal_abort_handler(0);
}

void EnableCrashingOnCrashes() {
	typedef BOOL(WINAPI* tGetPolicy)(LPDWORD lpFlags);
	typedef BOOL(WINAPI* tSetPolicy)(DWORD dwFlags);
	const DWORD EXCEPTION_SWALLOWING = 0x1;

	HMODULE kernel32 = LoadLibraryA("kernel32.dll");
	if(kernel32) {
		tGetPolicy pGetPolicy = (tGetPolicy)GetProcAddress(kernel32, "GetProcessUserModeExceptionPolicy");
		tSetPolicy pSetPolicy = (tSetPolicy)GetProcAddress(kernel32, "SetProcessUserModeExceptionPolicy");
		if(pGetPolicy && pSetPolicy) {
			DWORD dwFlags;
			if(pGetPolicy(&dwFlags)) {
				// Turn off the filter
				pSetPolicy(dwFlags & ~EXCEPTION_SWALLOWING);
			}
		}
	}
	BOOL insanity = FALSE;
	SetUserObjectInformationA(GetCurrentProcess(), UOI_TIMERPROC_EXCEPTION_SUPPRESSION, &insanity, sizeof(insanity));
}

int WINAPI wWinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/, LPWSTR /*commandline*/, int /*nCmdShow*/
) {

#ifdef _DEBUG
	HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
#endif

	InitializeCriticalSection(&guard_abort_handler);

	if(!IsDebuggerPresent()) {
		EnableCrashingOnCrashes();
		_set_purecall_handler(generic_wrapper);
		_set_invalid_parameter_handler(invalid_parameter_wrapper);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		SetUnhandledExceptionFilter(uef_wrapper);
		signal(SIGABRT, signal_abort_handler);
	}

	SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	if(SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
		// do everything here: create a window, read messages

		int num_params = 0;
		auto parsed_cmd = CommandLineToArgvW(GetCommandLineW(), &num_params);

		int headless_speed = 6;
		bool headless_repeat = false;
		bool headless = false;

		network::port_forwarder forwarding_apparatus;

		if(num_params < 2) {
#ifdef NDEBUG
			auto msg = std::string("Start Alice.exe using the launcher");
			window::emit_error_message(msg, true);
			return 0;
#else
			
			add_root(game_state.common_fs, NATIVE("."));				 // for the moment this lets us find the shader files
			

			if(!sys::try_read_scenario_and_save_file(game_state, NATIVE("development_test_file.bin"))) {
				// scenario making functions
				parsers::error_handler err{ "" };

				simple_fs::file_system fs_root;
				simple_fs::add_root(fs_root, L".");

				auto root = get_root(fs_root);
				auto common = open_directory(root, NATIVE("common"));
				auto save_dir = simple_fs::get_mod_save_dir_name(fs_root);

				parsers::bookmark_context bookmark_context;
				if(auto f = open_file(common, NATIVE("bookmarks.txt")); f) {
					auto bookmark_content = simple_fs::view_contents(*f);
					err.file_name = "bookmarks.txt";
					parsers::token_generator gen(bookmark_content.data, bookmark_content.data + bookmark_content.file_size);
					parsers::parse_bookmark_file(gen, err, bookmark_context);
					assert(!bookmark_context.bookmark_dates.empty());
				} else {
					err.accumulated_errors += "File common/bookmarks.txt could not be opened\n";
				}

				auto to_hex = [](uint64_t v) {
					native_string ret;
					constexpr native_char digits[] = NATIVE("0123456789ABCDEF");
					do {
						ret += digits[v & 0x0F];
						v = v >> 4;
					} while(v != 0);
					return ret;
				};

				sys::checksum_key scenario_key;
				for(uint32_t date_index = 0; date_index < uint32_t(bookmark_context.bookmark_dates.size()); date_index++) {
					err.accumulated_errors.clear();
					err.accumulated_warnings.clear();
					//
					auto inner_game_state = std::make_unique<sys::state>();
					inner_game_state->mod_save_dir = save_dir;
					simple_fs::add_root(inner_game_state->common_fs, L".");

					inner_game_state->load_scenario_data(err, bookmark_context.bookmark_dates[date_index].date_);
					if(err.fatal)
						break;
					if(date_index == 0) {
						auto sdir = simple_fs::get_or_create_scenario_directory();
						int32_t append = 0;
						auto time_stamp = uint64_t(std::time(0));
						auto selected_scenario_file = native_string(NATIVE("development_test_file.bin"));
						sys::write_scenario_file(*inner_game_state, selected_scenario_file, 0);
						if(auto of = simple_fs::open_file(sdir, selected_scenario_file); of) {
							auto content = view_contents(*of);
							auto desc = sys::extract_mod_information(reinterpret_cast<uint8_t const*>(content.data), content.file_size);
						}
						scenario_key = inner_game_state->scenario_checksum;
					} else {
						inner_game_state->scenario_checksum = scenario_key;
						sys::write_save_file(*inner_game_state, sys::save_type::bookmark, bookmark_context.bookmark_dates[date_index].name_);
					}
				}

				if(!err.accumulated_errors.empty()) {
					auto assembled_file = std::string("You can still play the mod, but it might be unstable\r\nThe following problems were encountered while creating the scenario:\r\n\r\nErrors:\r\n") + err.accumulated_errors + "\r\n\r\nWarnings:\r\n" + err.accumulated_warnings;
					auto pdir = simple_fs::get_or_create_settings_directory();
					simple_fs::write_file(pdir, NATIVE("scenario_errors.txt"), assembled_file.data(), uint32_t(assembled_file.length()));

					auto fname = simple_fs::get_full_name(pdir) + NATIVE("\\scenario_errors.txt");
					ShellExecuteW(
							nullptr,
							L"open",
							fname.c_str(),
							nullptr,
							nullptr,
							SW_NORMAL
						);
					
					std::abort();
				}
				if(!sys::try_read_scenario_and_save_file(game_state, NATIVE("development_test_file.bin")))
					std::abort();
			}

			game_state.fill_unsaved_data();
#endif
		} else {
			for(int i = 1; i < num_params; ++i) {
				if(native_string(parsed_cmd[i]) == NATIVE("-host")) {
					network::save_host_settings(game_state);
					network::load_host_settings(game_state);
					forwarding_apparatus.start_forwarding(game_state.host_settings.alice_host_port);
					game_state.network_mode = sys::network_mode_type::host;
				} else if(native_string(parsed_cmd[i]) == NATIVE("-join")) {
					game_state.network_mode = sys::network_mode_type::client;
					game_state.network_state.ip_address = "127.0.0.1";
					game_state.network_state.port = "1984";
					if(i + 1 < num_params) {
						auto native_param = native_string(parsed_cmd[i + 1]);
						auto semicolon_pos = native_param.find(NATIVE(";"));
						if(semicolon_pos != native_string::npos) {
							if(semicolon_pos + 1 >= native_param.length()) {
								window::emit_error_message("Missing port number after semicolon", true);
							}
							game_state.network_state.ip_address = simple_fs::native_to_utf8(native_param.substr(0, semicolon_pos));
							game_state.network_state.port = simple_fs::native_to_utf8(native_param.substr(semicolon_pos + 1));
						}
						else {
							game_state.network_state.ip_address = simple_fs::native_to_utf8(native_param);
						}
						i++;
					}
				} else if(native_string(parsed_cmd[i]) == NATIVE("-name")) {
					if(i + 1 < num_params) {
						std::string nickname = simple_fs::native_to_utf8(native_string(parsed_cmd[i + 1]));
						memcpy(&game_state.network_state.nickname.data, nickname.c_str(), std::min<size_t>(nickname.length(), sizeof(game_state.network_state.nickname.data)));
						i++;
					}
				} else if(native_string(parsed_cmd[i]) == NATIVE("-password")) {
					if(i + 1 < num_params) {
						auto str = simple_fs::native_to_utf8(native_string(parsed_cmd[i + 1]));
						std::memset(game_state.network_state.lobby_password, '\0', sizeof(game_state.network_state.lobby_password));
						std::memcpy(game_state.network_state.lobby_password, str.c_str(), std::min(sizeof(game_state.network_state.lobby_password), str.length()));
						i++;
					}
				}
				else if (native_string(parsed_cmd[i]) == NATIVE("-player_password")) {
					if (i + 1 < num_params) {
						std::string password = simple_fs::native_to_utf8(native_string(parsed_cmd[i + 1]));
						memcpy(&game_state.network_state.player_password.data, password.c_str(), std::min<size_t>(password.length(), 8));
						i++;
					}
				}
				else if(native_string(parsed_cmd[i]) == NATIVE("-v6")) {
					game_state.network_state.as_v6 = true;
				} else if(native_string(parsed_cmd[i]) == NATIVE("-v4")) {
					game_state.network_state.as_v6 = false;
				} else if(native_string(parsed_cmd[i]) == NATIVE("-headless")) {
					headless = true;
				} else if(native_string(parsed_cmd[i]) == NATIVE("-repeat")) {
					headless_repeat = true;
				} else if(native_string(parsed_cmd[i]) == NATIVE("-speed")) {
					if(i + 1 < num_params) {
						auto str = simple_fs::native_to_utf8(native_string(parsed_cmd[i + 1]));
						headless_speed = std::atoi(str.c_str());
						i++;
					}
				}
			}

			if(sys::try_read_scenario_and_save_file(game_state, parsed_cmd[1])) {
				game_state.fill_unsaved_data();
				game_state.console_log("Read scenario file " + simple_fs::native_to_utf8(parsed_cmd[1]));
			} else {
				auto msg = std::string("Scenario file ") + simple_fs::native_to_utf8(parsed_cmd[1]) + " could not be read";
				window::emit_error_message(msg, true);
				return 0;
			}
		}
		LocalFree(parsed_cmd);

		// scenario loading functions (would have to run these even when scenario is pre-built)
		game_state.load_user_settings();
		ui::populate_definitions_map(game_state);

		if(game_state.network_mode == sys::network_mode_type::host) {

			if(game_state.host_settings.alice_expose_webui != 0) {
				std::thread web_thread([&]() { webui::init(game_state); });
				web_thread.detach();
			}
		}

		network::init(game_state);

		if(headless) {
			game_state.actual_game_speed = headless_speed;
			game_state.ui_pause.store(false, std::memory_order::release);
			game_scene::switch_scene(game_state, game_scene::scene_id::in_game_basic);
			game_state.local_player_nation = dcon::nation_id{};
			// headless: network::init gave the rank-1 nation to a local player; nobody plays it, so hand it back to the AI
			for(auto n : game_state.world.in_nation)
				n.set_is_player_controlled(false);
			if(headless_repeat) {
				std::thread update_thread([&]() { game_state.game_loop(); });
				while(!game_state.quit_signaled.load(std::memory_order::acquire)) {
					while(game_state.current_scene.game_in_progress) {
						std::this_thread::sleep_for(std::chrono::milliseconds(15));
					}
					//Reload savefile
					network::finish(game_state, true);
					game_state.actual_game_speed = 0;
					//
					if(sys::try_read_scenario_and_save_file(game_state, parsed_cmd[1])) {
						game_state.fill_unsaved_data();
					} else {
						auto msg = std::string("Scenario file ") + simple_fs::native_to_utf8(parsed_cmd[1]) + " could not be read";
						window::emit_error_message(msg, true);
						return 0;
					}
					//
					network::init(game_state);
					game_scene::switch_scene(game_state, game_scene::scene_id::in_game_basic);
					game_state.actual_game_speed = headless_speed;
				};
				update_thread.join();
			} else {
				game_state.game_loop();
			}
		} else {
			std::thread update_thread([&]() { 
				game_state.game_loop(); 
			});
			std::thread ui_cache_update([&]() {
				game_state.ui_cached_data.process_update(game_state); 
			});
			std::thread graphics_cache_update([&](){
				game_state.map_state.update_cache(game_state);
			});
			std::thread map_labels_update([&]() {
				game_state.map_state.update_map_labels(game_state);
			});

			// entire game runs during this line
			window::create_window(game_state, window::creation_parameters{ 1024, 780, window::window_state::maximized, game_state.user_settings.prefer_fullscreen });
			game_state.quit_signaled.store(true, std::memory_order_release);

			update_thread.join();
			ui_cache_update.join();
			graphics_cache_update.join();
			map_labels_update.join();
		}

		network::finish(game_state, true);
		webui::svr.stop();
		CoUninitialize();
	}
	return 0;
}
