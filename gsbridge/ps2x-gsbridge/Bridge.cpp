#include <string_view>
// ps2x-gsbridge: PCSX2-GS als Bibliothek fuer einen nativen PS2-Port (SotC Recomp).
// Abgeleitet von pcsx2-gsrunner/Main.cpp (PCSX2 Dev Team, GPL-3.0+): Host-Schnittstelle, Konfiguration und
// X11-Fenster bleiben, statt eines Dump-Files speist der Port Pakete live ein (GSDumpReplayer Live-Modus).
// SPDX-License-Identifier: GPL-3.0+
// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

#include "fmt/format.h"

#include "common/Assertions.h"
#include "common/CocoaTools.h"
#include "common/Console.h"
#include "common/CrashHandler.h"
#include "common/FileSystem.h"
#include "common/MemorySettingsInterface.h"
#include "common/Path.h"
#include "common/ProgressCallback.h"
#include "common/SettingsWrapper.h"
#include "common/StringUtil.h"

#include "pcsx2/PrecompiledHeader.h"

#include "pcsx2/Achievements.h"
#include "pcsx2/CDVD/CDVD.h"
#include "pcsx2/GS.h"
#include "pcsx2/GS/GSCapture.h"
#include "pcsx2/GS/Renderers/Common/GSDevice.h"
#include "pcsx2/GS/GSPerfMon.h"
#include "pcsx2/GSDumpReplayer.h"
#include "pcsx2/GameList.h"
#include "pcsx2/Host.h"
#include "pcsx2/INISettingsInterface.h"
#include "pcsx2/ImGui/FullscreenUI.h"
#include "pcsx2/ImGui/ImGuiFullscreen.h"
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/Input/InputManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/SIO/Pad/Pad.h"
#include "pcsx2/PerformanceMetrics.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/R3000A.h"
#include "pcsx2/IopHw.h"
#include "pcsx2/SPU2/spu2.h"
#include "pcsx2/SPU2/defs.h"

#include "svnrev.h"

// Down here because X11 has a lot of defines that can conflict
#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace GSRunner
{
	static void InitializeConsole();
	static bool InitializeConfig();
	static void SettingsOverride();
	static bool ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params);
	static void DumpStats();

	static bool CreatePlatformWindow();
	static void DestroyPlatformWindow();
	static std::optional<WindowInfo> GetPlatformWindowInfo();
	static void PumpPlatformMessages(bool forever = false);
	static void StopPlatformMessagePump();
} // namespace GSRunner

static constexpr u32 WINDOW_WIDTH = 1280;
static constexpr u32 WINDOW_HEIGHT = 960;

static MemorySettingsInterface s_settings_interface;

static std::string s_output_prefix;
static s32 s_loop_count = 1;
static std::optional<bool> s_use_window;
static bool s_no_console = false;

// Owned by the GS thread.
static u32 s_dump_frame_number = 0;
static u32 s_loop_number = s_loop_count;
static double s_last_internal_draws = 0;
static double s_last_draws = 0;
static double s_last_render_passes = 0;
static double s_last_barriers = 0;
static double s_last_copies = 0;
static double s_last_uploads = 0;
static double s_last_readbacks = 0;
static double s_last_depth_copies_rov = 0;
static double s_last_draws_rov = 0;
static double s_last_barriers_rov = 0;
static u64 s_total_internal_draws = 0;
static u64 s_total_draws = 0;
static u64 s_total_render_passes = 0;
static u64 s_total_barriers = 0;
static u64 s_total_copies = 0;
static u64 s_total_uploads = 0;
static u64 s_total_readbacks = 0;
static u64 s_total_copies_rov = 0;
static u64 s_total_draws_rov = 0;
static u64 s_total_barriers_rov = 0;
static u32 s_total_frames = 0;
static u32 s_total_drawn_frames = 0;

static bool s_perf_enable = false;
static float s_perf_updates = 0.0f;
static float s_perf_sum_fps = 0.0f;
static float s_perf_sum_internal_fps = 0.0f;
static float s_perf_sum_cpu_thread_usage = 0.0f;
static float s_perf_sum_cpu_thread_time = 0.0f;
static float s_perf_sum_gs_thread_usage = 0.0f;
static float s_perf_sum_gs_thread_time = 0.0f;
static float s_perf_sum_gpu_time = 0.0f;
static float s_perf_sum_gpu_usage = 0.0f;

bool GSRunner::InitializeConfig()
{
	EmuFolders::SetAppRoot();
	// ps2x: Ressourcen (Shader, Schriften, GameDB) liegen neben der Bibliothek, nicht neben dem Spielprogramm
	if (const char* root = std::getenv("PS2XGS_APPROOT"); root && *root)
		EmuFolders::AppRoot = root;
	if (!EmuFolders::SetResourcesDirectory() || !EmuFolders::SetDataDirectory(nullptr))
		return false;
#ifdef _WIN32
	// Eigener Datenordner statt Dokumente\PCSX2: eine installierte PCSX2-Version teilt sich sonst Shader-Cache,
	// Einstellungen und Ressourcen mit dem Port (anderes Format, andere Version). PS2XGS_DATAROOT ueberschreibt.
	{
		std::string dr;
		if (const char* e = std::getenv("PS2XGS_DATAROOT"); e && *e) dr = e;
		else if (const char* a = std::getenv("APPDATA"); a && *a)
		{
			const char* cn = std::getenv("PS2XGS_CFGNAME");
			dr = std::string(a) + "\\" + ((cn && *cn) ? cn : "ts2-recomp") + "\\pcsx2";
		}
		if (!dr.empty() && FileSystem::CreateDirectoryPath(dr.c_str(), false))
			EmuFolders::DataRoot = dr;
	}
#endif

	CrashHandler::SetWriteDirectory(EmuFolders::DataRoot);

	const char* error;
	if (!VMManager::PerformEarlyHardwareChecks(&error))
		return false;

	{
		const std::string roboto_path =
			EmuFolders::GetOverridableResourcePath("fonts" FS_OSPATH_SEPARATOR_STR "Roboto-Regular.ttf");
		const auto roboto_data = FileSystem::MapBinaryFileForRead(roboto_path.c_str());
		if (roboto_data.empty())
		{
			Console.ErrorFmt("Failed to load font file '{}'.", roboto_path);
			return false;
		}

		std::vector<ImGuiManager::FontInfo> fonts;
		ImGuiManager::FontInfo fi{};
		fi.data = roboto_data;
		fi.exclude_ranges = {};
		fi.face_name = nullptr;
		fi.is_emoji_font = false;
		fonts.push_back(fi);

		ImGuiManager::SetFonts(std::move(fonts));
	}

	// don't provide an ini path, or bother loading. we'll store everything in memory.
	MemorySettingsInterface& si = s_settings_interface;
	Host::Internal::SetBaseSettingsLayer(&si);

	VMManager::SetDefaultSettings(si, true, true, true, true, true);

	VMManager::Internal::LoadStartupSettings();
	return true;
}

void Host::CommitBaseSettingChanges()
{
	// nothing to save, we're all in memory
}

void Host::LoadSettings(SettingsInterface& si, std::unique_lock<std::mutex>& lock)
{
}

void Host::CheckForSettingsChanges(const Pcsx2Config& old_config)
{
}

bool Host::RequestResetSettings(bool folders, bool core, bool controllers, bool hotkeys, bool ui)
{
	// not running any UI, so no settings requests will come in
	return false;
}

void Host::SetDefaultUISettings(SettingsInterface& si)
{
	// nothing
}

bool Host::LocaleCircleConfirm()
{
	// not running any UI, so no settings requests will come in
	return false;
}

std::unique_ptr<ProgressCallback> Host::CreateHostProgressCallback()
{
	return ProgressCallback::CreateNullProgressCallback();
}

void Host::ReportInfoAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		INFO_LOG("ReportInfoAsync: {}: {}", title, message);
	else if (!message.empty())
		INFO_LOG("ReportInfoAsync: {}", message);
}

void Host::ReportErrorAsync(const std::string_view title, const std::string_view message)
{
	if (!title.empty() && !message.empty())
		ERROR_LOG("ReportErrorAsync: {}: {}", title, message);
	else if (!message.empty())
		ERROR_LOG("ReportErrorAsync: {}", message);
}

void Host::OpenURL(const std::string_view url)
{
	// noop
}

bool Host::CopyTextToClipboard(const std::string_view text)
{
	return false;
}

std::string Host::GetTextFromClipboard()
{
	return std::string();
}

void Host::BeginTextInput()
{
	// noop
}

void Host::EndTextInput()
{
	// noop
}

std::optional<WindowInfo> Host::GetTopLevelWindowInfo()
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::OnInputDeviceConnected(const std::string_view identifier, const std::string_view device_name)
{
}

void Host::OnInputDeviceDisconnected(const InputBindingKey key, const std::string_view identifier)
{
}

void Host::SetMouseMode(bool relative_mode, bool hide_cursor)
{
}

void Host::SetMouseLock(bool state)
{
}

std::optional<WindowInfo> Host::AcquireRenderWindow(bool recreate_window)
{
	return GSRunner::GetPlatformWindowInfo();
}

void Host::ReleaseRenderWindow()
{
}

void Host::BeginPresentFrame()
{
	if (s_loop_number == 0 && !s_output_prefix.empty())
	{
		// when we wrap around, don't race other files
		GSJoinSnapshotThreads();

		// queue dumping of this frame
		std::string dump_path(fmt::format("{}_frame{:05}.png", s_output_prefix, s_dump_frame_number));
		GSQueueSnapshot(dump_path);
	}

	if (GSIsHardwareRenderer())
	{
		const u32 last_draws = s_total_internal_draws;
		const u32 last_uploads = s_total_uploads;

		static constexpr auto update_stat = [](GSPerfMon::counter_t counter, u64& dst, double& last) {
			// perfmon resets every 30 frames to zero
			const double val = g_perfmon.GetCounter(counter);
			dst += static_cast<u64>((val < last) ? val : (val - last));
			last = val;
		};

		update_stat(GSPerfMon::Draw, s_total_internal_draws, s_last_internal_draws);
		update_stat(GSPerfMon::DrawCalls, s_total_draws, s_last_draws);
		update_stat(GSPerfMon::RenderPasses, s_total_render_passes, s_last_render_passes);
		update_stat(GSPerfMon::Barriers, s_total_barriers, s_last_barriers);
		update_stat(GSPerfMon::TextureCopies, s_total_copies, s_last_copies);
		update_stat(GSPerfMon::TextureUploads, s_total_uploads, s_last_uploads);
		update_stat(GSPerfMon::Readbacks, s_total_readbacks, s_last_readbacks);
		update_stat(GSPerfMon::TextureCopiesROV, s_total_copies_rov, s_last_depth_copies_rov);
		update_stat(GSPerfMon::DrawCallsROV, s_total_draws_rov, s_last_draws_rov);
		update_stat(GSPerfMon::BarriersROV, s_total_barriers_rov, s_last_barriers_rov);

		const bool idle_frame = s_total_frames && (last_draws == s_total_internal_draws && last_uploads == s_total_uploads);

		if (!idle_frame)
			s_total_drawn_frames++;

		s_total_frames++;

		// PS2XGS_STAT=1: alle 50 Bilder die PCSX2-Zaehler ausgeben (Draws, Render-Paesse, Barrieren, Kopien, Uploads, Rueckleser)
		static const bool statOn = [] { const char* e = std::getenv("PS2XGS_STAT"); return e && *e == '1'; }();
		if (statOn && (s_total_frames % 50) == 0)
			Console.WriteLn("[ps2xgs-stat] frames=%llu draws=%llu calls=%llu passes=%llu barriers=%llu copies=%llu uploads=%llu readbacks=%llu",
				(unsigned long long)s_total_frames, (unsigned long long)s_total_internal_draws, (unsigned long long)s_total_draws,
				(unsigned long long)s_total_render_passes, (unsigned long long)s_total_barriers, (unsigned long long)s_total_copies,
				(unsigned long long)s_total_uploads, (unsigned long long)s_total_readbacks);

		std::atomic_thread_fence(std::memory_order_release);
	}
}

void Host::RequestResizeHostDisplay(s32 width, s32 height)
{
}

void Host::OnVMStarting()
{
}

void Host::OnVMStarted()
{
}

void Host::OnVMDestroyed()
{
}

void Host::OnVMPaused()
{
}

void Host::OnVMResumed()
{
}

void Host::OnGameChanged(const std::string& title, const std::string& elf_override, const std::string& disc_path,
	const std::string& disc_serial, u32 disc_crc, u32 current_crc)
{
}

void Host::OnPerformanceMetricsUpdated()
{
	// PS2XGS_PERFLOG=1: echte Spiel-Bildrate (wie die FPS-Anzeige) und VBlanks/s protokollieren
	static const bool perflog = std::getenv("PS2XGS_PERFLOG") != nullptr;
	if (perflog)
		Console.WriteLn(fmt::format("[ps2xperf] FPS {:.2f} ({}) | VPS {:.2f} | Bildzeit {:.2f} ms", PerformanceMetrics::GetInternalFPS(),
			PerformanceMetrics::GetInternalFPSMethod() == PerformanceMetrics::InternalFPSMethod::GSPrivilegedRegister ? "P" :
			PerformanceMetrics::GetInternalFPSMethod() == PerformanceMetrics::InternalFPSMethod::DISPFBBlit ? "B" : "-",
			PerformanceMetrics::GetFPS(), PerformanceMetrics::GetAverageFrameTime()));
	if (s_perf_enable)
	{
		s_perf_updates += 1.0f;
		s_perf_sum_fps += PerformanceMetrics::GetFPS();
		s_perf_sum_internal_fps += PerformanceMetrics::GetInternalFPS();
		s_perf_sum_cpu_thread_usage += PerformanceMetrics::GetCPUThreadUsage();
		s_perf_sum_cpu_thread_time += PerformanceMetrics::GetCPUThreadAverageTime();
		s_perf_sum_gs_thread_usage += PerformanceMetrics::GetGSThreadUsage();
		s_perf_sum_gs_thread_time += PerformanceMetrics::GetGSThreadAverageTime();
		s_perf_sum_gpu_time += PerformanceMetrics::GetGPUAverageTime();
		s_perf_sum_gpu_usage += PerformanceMetrics::GetGPUUsage();
	}
}

void Host::OnSaveStateLoading(const std::string_view filename)
{
}

void Host::OnSaveStateLoaded(const std::string_view filename, bool was_successful)
{
}

void Host::OnSaveStateSaved(const std::string_view filename)
{
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	pxFailRel("Not implemented");
}

void Host::RefreshGameListAsync(bool invalidate_cache)
{
}

void Host::CancelGameListRefresh()
{
}

bool Host::IsFullscreen()
{
	return false;
}

void Host::SetFullscreen(bool enabled)
{
}

void Host::OnCaptureStarted(const std::string& filename)
{
}

void Host::OnCaptureStopped()
{
}

void Host::RequestExitApplication(bool allow_confirm)
{
}

void Host::RequestExitBigPicture()
{
}

void Host::RequestVMShutdown(bool allow_confirm, bool allow_save_state, bool default_save_state)
{
	VMManager::SetState(VMState::Stopping);
}

void Host::OnAchievementsLoginSuccess(const char* username, u32 points, u32 sc_points, u32 unread_messages)
{
	// noop
}

void Host::OnAchievementsLoginRequested(Achievements::LoginRequestReason reason)
{
	// noop
}

void Host::OnAchievementsHardcoreModeChanged(bool enabled)
{
	// noop
}

void Host::OnAchievementsRefreshed()
{
	// noop
}

bool Host::InBatchMode()
{
	return false;
}

bool Host::InNoGUIMode()
{
	return false;
}

bool Host::ShouldPreferHostFileSelector()
{
	return false;
}

void Host::OpenHostFileSelectorAsync(std::string_view title, bool select_directory, FileSelectorCallback callback,
	FileSelectorFilters filters, std::string_view initial_directory)
{
	callback(std::string());
}

int Host::LocaleSensitiveCompare(std::string_view lhs, std::string_view rhs)
{
	const int res = std::strncmp(lhs.data(), rhs.data(), std::min(lhs.size(), rhs.size()));
	if (res != 0)
		return res;
	return lhs.size() > rhs.size() ? 1 : (lhs.size() < rhs.size() ? -1 : 0);
}

std::optional<u32> InputManager::ConvertHostKeyboardStringToCode(const std::string_view str)
{
	return std::nullopt;
}

std::optional<std::string> InputManager::ConvertHostKeyboardCodeToString(u32 code)
{
	return std::nullopt;
}

const char* InputManager::ConvertHostKeyboardCodeToIcon(u32 code)
{
	return nullptr;
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()

static void PrintCommandLineVersion()
{
	std::fprintf(stderr, "PCSX2 GS Runner Version %s\n", GIT_REV);
	std::fprintf(stderr, "https://pcsx2.net/\n");
	std::fprintf(stderr, "\n");
}

static void PrintCommandLineHelp(const char* progname)
{
	PrintCommandLineVersion();
	std::fprintf(stderr, "Usage: %s [parameters] [--] [filename]\n", progname);
	std::fprintf(stderr, "\n");
	std::fprintf(stderr, "  -help: Displays this information and exits.\n");
	std::fprintf(stderr, "  -version: Displays version information and exits.\n");
	std::fprintf(stderr, "  -dumpdir <dir>: Frame dump directory (will be dumped as filename_frameN.png).\n");
	std::fprintf(stderr, "  -dump [rt|tex|z|f|a|i|tr|ds|fs|hw]: Enabling dumping of render target, texture, z buffer, frame, "
		"alphas, and info (context, vertices, list of transfers), transfers images, draw stats, frame stats, HW config, respectively, per draw. Generates lots of data.\n");
	std::fprintf(stderr, "  -dumprange N[,L,B]: Start dumping from draw N (base 0), stops after L draws, and only "
		"those draws that are multiples of B (intersection of -dumprange and -dumprangef used)."
		"Defaults to 0,-1,1 (all draws). Only used if -dump used.\n");
	std::fprintf(stderr, "  -dumprangef NF[,LF,BF]: Start dumping from frame NF (base 0), stops after LF frames, "
		"and only those frames that are multiples of BF (intersection of -dumprange and -dumprangef used).\n"
		"Defaults to 0,-1,1 (all frames). Only used if -dump is used.\n");
	std::fprintf(stderr, "  -loop <count>: Loops dump playback N times. Defaults to 1. 0 will loop infinitely.\n");
	std::fprintf(stderr, "  -renderer <renderer>: Sets the graphics renderer. Defaults to Auto.\n");
	std::fprintf(stderr, "  -swthreads <threads>: Sets the number of threads for the software renderer.\n");
	std::fprintf(stderr, "  -window: Forces a window to be displayed.\n");
	std::fprintf(stderr, "  -surfaceless: Disables showing a window.\n");
	std::fprintf(stderr, "  -logfile <filename>: Writes emu log to filename.\n");
	std::fprintf(stderr, "  -noshadercache: Disables the shader cache (useful for parallel runs).\n");
	std::fprintf(stderr, "  -perf: Enable frame timing performance stats.\n");
	std::fprintf(stderr, "  --: Signals that no more arguments will follow and the remaining\n"
						 "    parameters make up the filename. Use when the filename contains\n"
						 "    spaces or starts with a dash.\n");
	std::fprintf(stderr, "\n");
}

void GSRunner::InitializeConsole()
{
	const char* var = std::getenv("PCSX2_NOCONSOLE");
	s_no_console = (var && StringUtil::FromChars<bool>(var).value_or(false));
	if (!s_no_console)
		Log::SetConsoleOutputLevel(LOGLEVEL_DEBUG);
}

bool GSRunner::ParseCommandLineArgs(int argc, char* argv[], VMBootParameters& params)
{
	std::string dumpdir; // Save from argument -dumpdir for creating sub-directories
	bool no_more_args = false;
	for (int i = 1; i < argc; i++)
	{
		if (!no_more_args)
		{
#define CHECK_ARG(str) !std::strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) (!std::strcmp(argv[i], str) && ((i + 1) < argc))

			if (CHECK_ARG("-help"))
			{
				PrintCommandLineHelp(argv[0]);
				return false;
			}
			else if (CHECK_ARG("-version"))
			{
				PrintCommandLineVersion();
				return false;
			}
			else if (CHECK_ARG_PARAM("-dumpdir"))
			{
				dumpdir = s_output_prefix = StringUtil::StripWhitespace(argv[++i]);
				if (s_output_prefix.empty())
				{
					Console.Error("Invalid dump directory specified.");
					return false;
				}

				if (!FileSystem::DirectoryExists(s_output_prefix.c_str()) && !FileSystem::CreateDirectoryPath(s_output_prefix.c_str(), false))
				{
					Console.Error("Failed to create output directory");
					return false;
				}

				continue;
			}
			else if (CHECK_ARG_PARAM("-dump"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "DumpGSData", true);

				if (str.find("rt") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveRT", true);
				if (str.find("f") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrame", true);
				if (str.find("tex") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTexture", true);
				if (str.find("z") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDepth", true);
				if (str.find("a") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveAlpha", true);
				if (str.find("i") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveInfo", true);
				if (str.find("tr") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveTransferImages", true);
				if (str.find("ds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveDrawStats", true);
				if (str.find("fs") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveFrameStats", true);
				if (str.find("hw") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "SaveHWConfig", true);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprange"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveDrawBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumprangef"))
			{
				std::string str(argv[++i]);

				std::vector<std::string_view> split = StringUtil::SplitString(str, ',');
				int start = 0;
				int num = -1;
				int by = 1;
				if (split.size() > 0)
				{
					start = StringUtil::FromChars<int>(split[0]).value_or(0);
				}
				if (split.size() > 1)
				{
					num = StringUtil::FromChars<int>(split[1]).value_or(-1);
				}
				if (split.size() > 2)
				{
					by = std::max(1, StringUtil::FromChars<int>(split[2]).value_or(1));
				}
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameStart", start);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameCount", num);
				s_settings_interface.SetIntValue("EmuCore/GS", "SaveFrameBy", by);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirhw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-dumpdirsw"))
			{
				s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", argv[++i]);
				continue;
			}
			else if (CHECK_ARG_PARAM("-loop"))
			{
				s_loop_count = StringUtil::FromChars<s32>(argv[++i]).value_or(0);
				Console.WriteLn("Looping dump playback %d times.", s_loop_count);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderer"))
			{
				const char* rname = argv[++i];

				GSRendererType type = GSRendererType::Auto;
				if (StringUtil::Strcasecmp(rname, "Auto") == 0)
					type = GSRendererType::Auto;
#ifdef _WIN32
				else if (StringUtil::Strcasecmp(rname, "dx11") == 0)
					type = GSRendererType::DX11;
				else if (StringUtil::Strcasecmp(rname, "dx12") == 0)
					type = GSRendererType::DX12;
#endif
#ifdef ENABLE_OPENGL
				else if (StringUtil::Strcasecmp(rname, "gl") == 0)
					type = GSRendererType::OGL;
#endif
#ifdef ENABLE_VULKAN
				else if (StringUtil::Strcasecmp(rname, "vulkan") == 0)
					type = GSRendererType::VK;
#endif
#ifdef __APPLE__
				else if (StringUtil::Strcasecmp(rname, "metal") == 0)
					type = GSRendererType::Metal;
#endif
				else if (StringUtil::Strcasecmp(rname, "sw") == 0)
					type = GSRendererType::SW;
				else
				{
					Console.Error("Unknown renderer '%s'", rname);
					return false;
				}

				Console.WriteLn("Using %s renderer.", Pcsx2Config::GSOptions::GetRendererName(type));
				s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", static_cast<int>(type));
				continue;
			}
			else if (CHECK_ARG_PARAM("-swthreads"))
			{
				const int swthreads = StringUtil::FromChars<int>(argv[++i]).value_or(0);
				if (swthreads < 0)
				{
					Console.WriteLn("Invalid number of software threads");
					return false;
				}
				
				Console.WriteLn(fmt::format("Setting number of software threads to {}", swthreads));
				s_settings_interface.SetIntValue("EmuCore/GS", "SWExtraThreads", swthreads);
				continue;
			}
			else if (CHECK_ARG_PARAM("-renderhacks"))
			{
				std::string str(argv[++i]);

				s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);

				if (str.find("af") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_AutoFlushLevel", 1);
				if (str.find("cpufb") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_CPU_FB_Conversion", true);
				if (str.find("dds") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisableDepthSupport", true);
				if (str.find("dpi") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_DisablePartialInvalidation", true);
				if (str.find("dsf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks_Disable_Safe_Features", true);
				if (str.find("tinrt") != std::string::npos)
					s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_TextureInsideRt", 1);
				if (str.find("plf") != std::string::npos)
					s_settings_interface.SetBoolValue("EmuCore/GS", "preload_frame_with_gs_data", true);

				continue;
			}
			else if (CHECK_ARG_PARAM("-ini"))
			{
				std::string path = std::string(StringUtil::StripWhitespace(argv[++i]));
				if (!FileSystem::FileExists(path.c_str()))
				{
					Console.ErrorFmt("INI file {} does not exit.", path);
					return false;
				}

				INISettingsInterface si_ini(path);

				if (!si_ini.Load())
				{
					Console.ErrorFmt("Unable to load INI settings from {}.", path);
					return false;
				}

				for (const auto& [key, value] : si_ini.GetKeyValueList("EmuCore/GS"))
					s_settings_interface.SetStringValue("EmuCore/GS", key.c_str(), value.c_str());

				continue;
			}
			else if (CHECK_ARG_PARAM("-upscale"))
			{
				const float upscale = StringUtil::FromChars<float>(argv[++i]).value_or(0.0f);
				if (upscale < 0.5f)
				{
					Console.WriteLn("Invalid upscale multiplier");
					return false;
				}

				Console.WriteLn(fmt::format("Setting upscale multiplier to {}", upscale));
				s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
				continue;
			}
			else if (CHECK_ARG_PARAM("-logfile"))
			{
				const char* logfile = argv[++i];
				if (std::strlen(logfile) > 0)
				{
					// disable timestamps, since we want to be able to diff the logs
					Console.WriteLn("Logging to %s...", logfile);
					VMManager::Internal::SetFileLogPath(logfile);
					s_settings_interface.SetBoolValue("Logging", "EnableFileLogging", true);
					s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", false);
				}

				continue;
			}
			else if (CHECK_ARG("-noshadercache"))
			{
				Console.WriteLn("Disabling shader cache");
				s_settings_interface.SetBoolValue("EmuCore/GS", "DisableShaderCache", true);
				continue;
			}
			else if (CHECK_ARG("-window"))
			{
				Console.WriteLn("Creating window");
				s_use_window = true;
				continue;
			}
			else if (CHECK_ARG("-surfaceless"))
			{
				Console.WriteLn("Running surfaceless");
				s_use_window = false;
				continue;
			}
			else if (CHECK_ARG("-perf"))
			{
				Console.WriteLn("Enable performance stats");
				s_perf_enable = true;
				continue;
			}
			else if (CHECK_ARG("-debugdevice"))
			{
				Console.WriteLn("Enable debug device");
				s_settings_interface.SetBoolValue("EmuCore/GS", "UseDebugDevice", true);
				continue;
			}
			else if (CHECK_ARG("--"))
			{
				no_more_args = true;
				continue;
			}
			else if (argv[i][0] == '-')
			{
				Console.Error("Unknown parameter: '%s'", argv[i]);
				return false;
			}

#undef CHECK_ARG
#undef CHECK_ARG_PARAM
		}

		if (!params.filename.empty())
			params.filename += ' ';
		params.filename += argv[i];
	}

	if (params.filename.empty())
	{
		Console.Error("No dump filename provided.");
		return false;
	}

	if (!VMManager::IsGSDumpFileName(params.filename))
	{
		Console.Error("Provided filename is not a GS dump.");
		return false;
	}

	if (s_settings_interface.GetBoolValue("EmuCore/GS", "DumpGSData") && !dumpdir.empty())
	{
		if (s_settings_interface.GetStringValue("EmuCore/GS", "HWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "HWDumpDirectory", dumpdir.c_str());
		if (s_settings_interface.GetStringValue("EmuCore/GS", "SWDumpDirectory").empty())
			s_settings_interface.SetStringValue("EmuCore/GS", "SWDumpDirectory", dumpdir.c_str());
		
		// Disable saving frames with SaveSnapshotToMemory()
		// Instead we save more "raw" snapshots when using -dump.
		s_output_prefix = "";
	}

	// set up the frame dump directory
	if (!s_output_prefix.empty())
	{
		// strip off all extensions
		std::string_view title(Path::GetFileTitle(params.filename));
		if (StringUtil::EndsWithNoCase(title, ".gs"))
			title = Path::GetFileTitle(title);

		s_output_prefix = Path::Combine(s_output_prefix, StringUtil::StripWhitespace(title));
		Console.WriteLn(fmt::format("Saving dumps as {}_frameN.png", s_output_prefix));
	}

	return true;
}

void GSRunner::SettingsOverride()
{
	// complete as quickly as possible
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "VsyncEnable", false);

	// Force screenshot quality settings to something more performant, overriding any defaults good for users.
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotFormat", static_cast<int>(GSScreenshotFormat::PNG));
	s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotQuality", 10);

	// ensure all input sources are disabled, we're not using them
	s_settings_interface.SetBoolValue("InputSources", "SDL", false);
	s_settings_interface.SetBoolValue("InputSources", "XInput", false);

	// we don't need any sound output
	s_settings_interface.SetStringValue("SPU2/Output", "OutputModule", "nullout");
	s_settings_interface.SetStringValue("SPU2/Output", "Backend", "Null"); // Ton nur im echten Spiel (ps2xgs_start)

	// none of the bindings are going to resolve to anything
	Pad::ClearPortBindings(s_settings_interface, 0);
	s_settings_interface.ClearSection("Hotkeys");

	// force logging
	s_settings_interface.SetBoolValue("Logging", "EnableSystemConsole", !s_no_console);
	s_settings_interface.SetBoolValue("Logging", "EnableTimestamps", true);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", true);

	// and show some stats :)
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowFPS", true);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowResolution", true);
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowGSStats", true);

	// remove memory cards, so we don't have sharing violations
	for (u32 i = 0; i < 2; i++)
	{
		s_settings_interface.SetBoolValue("MemoryCards", fmt::format("Slot{}_Enable", i + 1).c_str(), false);
		s_settings_interface.SetStringValue("MemoryCards", fmt::format("Slot{}_Filename", i + 1).c_str(), "");
	}
}

void GSRunner::DumpStats()
{
	std::atomic_thread_fence(std::memory_order_acquire);
	Console.WriteLn(fmt::format("======= HW STATISTICS FOR {} ({}) FRAMES ========", s_total_frames, s_total_drawn_frames));
	Console.WriteLn(fmt::format("@HWSTAT@ Draw Calls: {} (avg {})", s_total_draws, static_cast<u64>(std::ceil(s_total_draws / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Render Passes: {} (avg {})", s_total_render_passes, static_cast<u64>(std::ceil(s_total_render_passes / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers: {} (avg {})", s_total_barriers, static_cast<u64>(std::ceil(s_total_barriers / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies: {} (avg {})", s_total_copies, static_cast<u64>(std::ceil(s_total_copies / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Uploads: {} (avg {})", s_total_uploads, static_cast<u64>(std::ceil(s_total_uploads / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Readbacks: {} (avg {})", s_total_readbacks, static_cast<u64>(std::ceil(s_total_readbacks / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Copies (ROV): {} (avg {})", s_total_copies_rov, static_cast<u64>(std::ceil(s_total_copies_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Draws Calls (ROV): {} (avg {})", s_total_draws_rov, static_cast<u64>(std::ceil(s_total_draws_rov / static_cast<double>(s_total_drawn_frames)))));
	Console.WriteLn(fmt::format("@HWSTAT@ Barriers (ROV): {} (avg {})", s_total_barriers_rov, static_cast<u64>(std::ceil(s_total_barriers_rov / static_cast<double>(s_total_drawn_frames)))));
	if (s_perf_enable)
	{
		Console.WriteLn(fmt::format("@HWSTAT@ Minimum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMinimumFrameTime(), 1000.0f / PerformanceMetrics::GetMinimumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Average Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetAverageFrameTime(), 1000.0f / PerformanceMetrics::GetAverageFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ Maximum Frame Time: {:.3f} ms ({:.3f} FPS)", PerformanceMetrics::GetMaximumFrameTime(), 1000.0f / PerformanceMetrics::GetMaximumFrameTime()));
		Console.WriteLn(fmt::format("@HWSTAT@ CPU Thread Usage: {:.3f} %", s_perf_sum_cpu_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GS Thread Usage: {:.3f} %", s_perf_sum_gs_thread_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ GPU Usage: {:.3f} %", s_perf_sum_gpu_usage / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average CPU Thread Time: {:.3f} ms", s_perf_sum_cpu_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GS Thread Time: {:.3f} ms", s_perf_sum_gs_thread_time / s_perf_updates));
		Console.WriteLn(fmt::format("@HWSTAT@ Average GPU Time: {:.3f} ms", s_perf_sum_gpu_time / s_perf_updates));
	}
	Console.WriteLn("============================================");
}

#ifdef _WIN32
// We can't handle unicode in filenames if we don't use wmain on Win32.
#define main real_main
#endif

static void CPUThreadMain(VMBootParameters* params, std::atomic<int>* ret)
{
	ret->store(EXIT_FAILURE);

	if (VMManager::Internal::CPUThreadInitialize())
	{
		// apply new settings (e.g. pick up renderer change)
		VMManager::ApplySettings();
		GSDumpReplayer::SetIsDumpRunner(true);

		if (VMManager::Initialize(*params) == VMBootResult::StartupSuccess)
		{
			// run until end
			GSDumpReplayer::SetLoopCount(s_loop_count);
			VMManager::SetState(VMState::Running);
			if (s_perf_enable)
			{
				VMManager::SetLimiterMode(LimiterModeType::Unlimited);
				g_gs_device->SetGPUTimingEnabled(true);
			}
			while (VMManager::GetState() == VMState::Running)
				VMManager::Execute();
			VMManager::Shutdown(false);
			GSRunner::DumpStats();
			ret->store(EXIT_SUCCESS);
		}
	}

	VMManager::Internal::CPUThreadShutdown();
	GSRunner::StopPlatformMessagePump();
}

void Host::PumpMessagesOnCPUThread()
{
	// update GS thread copy of frame number
	MTGS::RunOnGSThread([frame_number = GSDumpReplayer::GetFrameNumber()]() { s_dump_frame_number = frame_number; });
	MTGS::RunOnGSThread([loop_number = GSDumpReplayer::GetLoopCount()]() { s_loop_number = loop_number; });
}

s32 Host::Internal::GetTranslatedStringImpl(
	const std::string_view context, const std::string_view msg, char* tbuf, size_t tbuf_space)
{
	if (msg.size() > tbuf_space)
		return -1;
	else if (msg.empty())
		return 0;

	std::memcpy(tbuf, msg.data(), msg.size());
	return static_cast<s32>(msg.size());
}

std::string Host::TranslatePluralToString(const char* context, const char* msg, const char* disambiguation, int count)
{
	TinyString count_str = TinyString::from_format("{}", count);

	std::string ret(msg);
	for (;;)
	{
		std::string::size_type pos = ret.find("%n");
		if (pos == std::string::npos)
			break;

		ret.replace(pos, pos + 2, count_str.view());
	}

	return ret;
}

//////////////////////////////////////////////////////////////////////////
// Platform specific code
//////////////////////////////////////////////////////////////////////////

#if defined(__APPLE__)

static void* s_window;
static WindowInfo s_wi;

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window, "Tried to create window when there already was one!");
	s_window = CocoaTools::CreateWindow("PCSX2 GS Runner", WINDOW_WIDTH, WINDOW_HEIGHT);
	CocoaTools::GetWindowInfoFromWindow(&s_wi, s_window);
	PumpPlatformMessages();
	return s_window;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_window) {
		CocoaTools::DestroyWindow(s_window);
		s_window = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	CocoaTools::RunCocoaEventLoop(forever);
}

void GSRunner::StopPlatformMessagePump()
{
	CocoaTools::StopMainThreadEventLoop();
}

#elif defined(_WIN32) || defined(__linux__)
#ifdef _WIN32
// ps2x Windows: gleiche Spielmenue-/Eingabelogik wie unter Linux, Fenster ueber Win32 (eigener UI-Thread)
#include <windowsx.h>
#include <direct.h>
#include <sys/stat.h>
using Display = void;
using Window = HWND;
using KeySym = unsigned long;
#ifndef None
#define None 0
#endif
static Display* s_display = nullptr; // = Fenster vorhanden (zeigt auf das HWND)
static Window s_window = None;
static int ps2x_mkdir(const char* p, int) { return _mkdir(p); }
static int ps2x_setenv(const char* k, const char* v, int ow) { if (!ow && std::getenv(k)) return 0; return _putenv_s(k, v); }
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
// X11-Tastensymbole (gleiche Werte), damit die gemeinsame Tastenlogik unveraendert bleibt
enum : KeySym { XK_BackSpace = 0xff08, XK_Tab = 0xff09, XK_Return = 0xff0d, XK_Escape = 0xff1b, XK_Left = 0xff51, XK_Up = 0xff52,
	XK_Right = 0xff53, XK_Down = 0xff54, XK_Print = 0xff61, XK_KP_Enter = 0xff8d, XK_F1 = 0xffbe, XK_F7 = 0xffc4, XK_F8 = 0xffc5, XK_F9 = 0xffc6, XK_F10 = 0xffc7,
	XK_F11 = 0xffc8, XK_F12 = 0xffc9, XK_Shift_L = 0xffe1, XK_Shift_R = 0xffe2, XK_space = 0x20,
	XK_A = 'A', XK_C = 'C', XK_D = 'D', XK_E = 'E', XK_F = 'F', XK_G = 'G', XK_I = 'I', XK_J = 'J', XK_K = 'K', XK_L = 'L', XK_Q = 'Q',
	XK_R = 'R', XK_S = 'S', XK_V = 'V', XK_W = 'W', XK_X = 'X', XK_Y = 'Y', XK_Z = 'Z',
	XK_a = 'a', XK_c = 'c', XK_d = 'd', XK_e = 'e', XK_f = 'f', XK_g = 'g', XK_i = 'i', XK_j = 'j', XK_k = 'k', XK_l = 'l', XK_q = 'q',
	XK_r = 'r', XK_s = 's', XK_v = 'v', XK_w = 'w', XK_x = 'x', XK_y = 'y', XK_z = 'z' };
#else
#include <sys/stat.h>
static Display* s_display = nullptr;
static Window s_window = None;
static int ps2x_mkdir(const char* p, int m) { return mkdir(p, static_cast<mode_t>(m)); }
static int ps2x_setenv(const char* k, const char* v, int ow) { return setenv(k, v, ow); }
#endif
static WindowInfo s_wi;
static std::atomic<bool> s_shutdown_requested{false};

// ---- ps2x: Spielmenue (ImGui im Spielfenster, Gamepad/Tastatur/Maus), Einstellungen in ~/.config/sotc-recomp ----
#include "imgui.h"
#include "common/HTTPDownloader.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#endif
#include "pcsx2/ImGui/ImGuiManager.h"
#include "pcsx2/MTGS.h"
#include "pcsx2/GS/Renderers/HW/GSTextureReplacements.h"
#include "pcsx2/GS/Renderers/Common/GSRenderer.h"
#include <filesystem>
#include <set>
#include <fstream>
#include <vector>
#include <fstream>
#include <sys/stat.h>
namespace ps2xosd { std::atomic<u32> btns{0}, axes{0x80808080u}, keyBtns{0}; } // zuletzt ans Spiel gelieferte Eingabe
// Hotkeys: F9 FPS-Anzeige, F10 Turbo (ohne Bildratenbegrenzung), F12 Screenshot; kurze Einblendung (Toast)
namespace ps2xhot
{
	std::atomic<bool> turbo{false};
	std::mutex tm;
	std::string toastText;
	std::chrono::steady_clock::time_point toastAt{};
	void toast(std::string t) { std::lock_guard<std::mutex> lk(tm); toastText = std::move(t); toastAt = std::chrono::steady_clock::now(); }
	// Videoaufnahme (PCSX2 GSCapture ueber ffmpeg): F8 an/aus, PS2XGS_CAPTURE=<datei.mp4> startet beim ersten Bild.
	// Interne Aufloesung, 60 Bilder/s, mit Ton. Codec/Bitrate: PS2XGS_CAPTURE_CODEC (Standard libx264), PS2XGS_CAPTURE_KBPS (Standard 40000).
	// Freie Kamera (Dev-Modus): F7 an/aus, PS2XGS_FREECAM=1 ab Start. Die Laufzeit fliegt dann den Spieler mit Maus + WASD
	// durch den Level (Space/Strg hoch/runter, Shift schneller), Spieleingabe ist eingefroren.
	std::atomic<bool> freecam{[] { const char* e = std::getenv("PS2XGS_FREECAM"); return e && *e == '1'; }()};
	std::atomic<bool> aufnahme{false};
	std::string aufnahmeDatei;
	void aufnahmeStart(std::string datei)
	{
		aufnahmeDatei = datei;
		aufnahme.store(true);
		// Direkt mit 59.94 Hz: im Live-Betrieb der Bruecke kennt der GS-Zustand den TV-Modus nicht immer (SMODE1 noch leer),
		// dann liefert GSBeginCapture fps = 0 und der Encoder lehnt ab ("timebase not set").
		MTGS::RunOnGSThread([datei]() {
			if (!g_gs_renderer) return;
			// Aufnahme-Einstellungen direkt in GSConfig (die Werte aus toSettings kommen hier nicht an)
			const auto envOr = [](const char* k, const char* d) { const char* e = std::getenv(k); return (e && *e) ? e : d; };
			GSConfig.VideoCaptureCodec = envOr("PS2XGS_CAPTURE_CODEC", "libx264");
			GSConfig.VideoCaptureBitrate = std::atoi(envOr("PS2XGS_CAPTURE_KBPS", "40000"));
			GSConfig.EnableVideoCaptureParameters = true;
			GSConfig.VideoCaptureParameters = envOr("PS2XGS_CAPTURE_PARAMS", "crf=16:preset=veryfast");
			GSConfig.CaptureContainer = "mp4";
			GSConfig.EnableVideoCapture = true;
			// Ton: aus in Testlaeufen ohne Taktung (PS2XGS_CAPTURE_AUDIO=0) - sonst laeuft der Ton dem Bild davon, der Tonpuffer
			// blockiert den Spiel-Thread in DeliverAudioPacket und nichts geht mehr (gdb-Stacks ts2_366)
			GSConfig.EnableAudioCapture = std::atoi(envOr("PS2XGS_CAPTURE_AUDIO", "1")) != 0;
			GSConfig.AudioCaptureCodec = "aac";
			GSConfig.AudioCaptureBitrate = 192;
#ifdef _WIN32
			// FFmpeg-DLLs liegen in <gs>\ffmpeg\ und NICHT im Suchpfad: gleichnamige DLLs im PATH koennten sonst von fremden
			// Modulen im Prozess (Overlays, Aufnahme-Hooks) geladen werden. Hier mit vollem Pfad vorladen; PCSX2 findet sie dann.
			{
				const std::string root = envOr("PS2XGS_APPROOT", ".");
				for (const char* n : {"avutil-60.dll", "swresample-6.dll", "swscale-9.dll", "avcodec-62.dll", "avformat-62.dll"})
				{
					const std::wstring w = StringUtil::UTF8StringToWideString(root + "\\ffmpeg\\" + n);
					LoadLibraryExW(w.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
				}
			}
#endif
			GSVector2i res = g_gs_renderer->GetInternalResolution();
			if (res.x <= 0 || res.y <= 0) // noch kein Bild: aus Skalierung x PS2-Bild (640x448) ableiten
				res = GSVector2i(static_cast<int>(640 * EmuConfig.GS.UpscaleMultiplier), static_cast<int>(448 * EmuConfig.GS.UpscaleMultiplier));
			// Aufnahmehoehe begrenzen (Standard 1080 Zeilen, PS2XGS_CAPTURE_HOEHE): in voller 6x/8x-Aufloesung schafft x264 keine
			// 60 Bilder/s, das Spiel wartet dann auf den Encoder (niedrige Bildrate, mit Ton blockiert der Tonpuffer)
			const int maxH = std::max(448, std::atoi(envOr("PS2XGS_CAPTURE_HOEHE", "1080")));
			if (res.y > maxH) { res.x = static_cast<int>(static_cast<long long>(res.x) * maxH / res.y); res.y = maxH; }
			res.x &= ~1; res.y &= ~1;
			const float aspekt = (EmuConfig.GS.AspectRatio == AspectRatioType::R16_9) ? 16.0f / 9.0f : (EmuConfig.GS.AspectRatio == AspectRatioType::Stretch ? 0.0f : 4.0f / 3.0f);
			GSConfig.VideoCaptureAutoResolution = false;
			GSConfig.VideoCaptureWidth = res.x; GSConfig.VideoCaptureHeight = res.y;
			GSCapture::BeginCapture(60.0f / 1.001f, res, aspekt, datei);
		});
		toast("Recording: " + datei + "  (F8 stops)");
	}
	// Beim Beenden: laufende Aufnahme sauber abschliessen (moov-Atom), sonst ist die Datei unlesbar
	void aufnahmeBeenden()
	{
		if (!aufnahme.exchange(false)) return;
		std::atomic<bool> fertig{false};
		MTGS::RunOnGSThread([&fertig]() { GSCapture::EndCapture(); fertig.store(true); });
		for (int i = 0; i < 600 && !fertig.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	void aufnahmeStopp()
	{
		aufnahme.store(false);
		MTGS::RunOnGSThread([]() { GSCapture::EndCapture(); });
		toast("Recording saved: " + aufnahmeDatei);
	}
	std::string shotDir()
	{
		if (const char* e = std::getenv("PS2XGS_SHOTDIR"); e && *e) return e;
#ifdef _WIN32
		const char* home = std::getenv("USERPROFILE");
		const std::string h = home ? home : ".";
#else
		const char* home = std::getenv("HOME");
		const std::string h = home ? home : "/tmp";
#endif
		struct stat sb{};
		for (const char* sub : {"/Pictures", "/Bilder"})
			if (stat((h + sub).c_str(), &sb) == 0 && S_ISDIR(sb.st_mode)) { const std::string d = h + sub + "/TimeSplitters 2"; ps2x_mkdir(d.c_str(), 0755); return d; }
		const std::string d = h + "/.local/share/ts2-recomp-screenshots"; ps2x_mkdir(d.c_str(), 0755); return d;
	}
}
// Steam Deck / SteamOS-Spielmodus erkennen (Steam setzt SteamDeck=1; sonst Board-Hersteller "Valve")
static bool ps2xAufSteamDeck()
{
	static const bool deck = [] {
		if (const char* e = std::getenv("SteamDeck"); e && *e == '1') return true;
#ifndef _WIN32
		if (std::FILE* f = std::fopen("/sys/devices/virtual/dmi/id/board_vendor", "r"))
		{
			char b[32] = {};
			const size_t n = std::fread(b, 1, sizeof(b) - 1, f);
			std::fclose(f);
			return n >= 5 && std::string_view(b, 5) == "Valve";
		}
#endif
		return false;
	}();
	return deck;
}

namespace ps2xmenu
{
	std::atomic<bool> open{false};
	std::atomic<bool> applyPending{false};
	std::atomic<bool> fsPending{false};
	std::atomic<u32> nav{0};          // gehaltene Menue-Tasten: 1 hoch 2 runter 4 links 8 rechts 16 OK 32 zurueck
	std::atomic<int> lernAktion{-1}, lernPlatz{0};   // Belegung: naechste Taste/Maustaste fuer diese Aktion
	std::atomic<double> lernAb{0.0};                  // ImGui-Zeit, ab der Eingaben zaehlen (nicht der eigene Klick)
	std::atomic<int> lernCode{0};                     // vom Fenster-Thread erfasster Code (0 = noch keiner, -1 = Abbruch)
	std::atomic<int> mouseX{-1}, mouseY{-1};
	std::atomic<u32> mouseBtn{0};
	std::atomic<int> wheel{0};
	std::atomic<bool> latch{false};   // nach dem Schliessen: Spiel-Eingabe erst nach Loslassen
	std::atomic<bool> gameWide{false};
	std::atomic<bool> active{false};   // Menue/Einstellungen aktiv (nicht in Testlaeufen ohne Fenster) // Spiel rendert im eigenen 16:9-Modus (set_screen_wide_and_progressive)

	// Spielbezogene Texte per Umgebung (Startskript des jeweiligen Ports), Standard: SotC
	const char* envOr(const char* k, const char* d) { const char* e = std::getenv(k); return (e && *e) ? e : d; }
	const char* spielTitel() { return envOr("PS2XGS_TITEL", "SHADOW OF THE COLOSSUS"); }
	const char* cfgName() { return envOr("PS2XGS_CFGNAME", "sotc-recomp"); }
	const char* startName() { return envOr("PS2XGS_STARTNAME", "the launcher"); }
	const char* speicherHinweis() { return envOr("PS2XGS_SPEICHERHINWEIS", "Saving works in the game as on the PS2."); }

	// ---- Menuesprache: Quelltexte englisch, deutsche Uebersetzung per Tabelle (Einstellung "sprache", -1 = automatisch) ----
	std::atomic<int> sprache{0}; // 0 English, 1 Deutsch
	int spracheAuto()
	{
#ifdef _WIN32
		return (GetUserDefaultUILanguage() & 0x3FF) == 0x07 ? 1 : 0;
#else
		const char* l = std::getenv("LC_ALL"); if (!l || !*l) l = std::getenv("LC_MESSAGES"); if (!l || !*l) l = std::getenv("LANG");
		return (l && l[0] == 'd' && l[1] == 'e') ? 1 : 0;
#endif
	}
	const char* tr(const char* en)
	{
		if (sprache.load() != 1) return en;
		static const std::unordered_map<std::string_view, const char*> de = {
			{"Settings", "Einstellungen"}, {"Display", "Anzeige"}, {"Picture", "Bild"}, {"Audio", "Ton"}, {"Controls", "Steuerung"},
			{"Textures", "Texturen"}, {"Online", "Online"}, {"Save data", "Speicherstand"}, {"Language", "Sprache"},
			{"Preset", "Voreinstellung"}, {"Remaster (sharp)", "Remaster (scharf)"}, {"Original look", "Original-Look"}, {"Performance", "Leistung"},
			{"Internal resolution", "Interne Aufloesung"}, {"Aspect ratio", "Bildformat"}, {"Takes full effect after the next area change or restart.", "Wirkt ab dem naechsten Bereichswechsel bzw. Neustart voll."},
			{"Texture filtering", "Texturfilter"}, {"Anisotropic filtering", "Anisotrope Filterung"}, {"Trilinear filtering", "Trilineare Filterung"},
			{"Fullscreen", "Vollbild"}, {"V-Sync", "V-Sync"}, {"Show FPS", "FPS-Anzeige"}, {"Open this menu at start", "Menue beim Spielstart zeigen"}, {"Input overlay (debug)", "Eingabe-Anzeige (Debug)"},
			{"Resolution & filtering", "Aufloesung & Filter"}, {"Window", "Fenster"},
			{"Original frame blending (ghosting when turning)", "Nachzieheffekt beim Drehen (wie Original)"},
			{"The PS2 game blends the previous frame half over the new one. Off = clear picture.", "Das Original blendet das vorige Bild halb ueber das neue (Geisterbilder). Aus = klares Bild."},
			{"Anti-aliasing (FXAA)", "Kantenglaettung (FXAA)"}, {"Sharpening (AMD CAS)", "Schaerfe (AMD CAS)"}, {"Sharpening strength", "Schaerfe-Staerke"},
			{"PS2 dithering", "PS2-Dithering"}, {"TV / CRT filter", "Bildfilter (TV/CRT)"}, {"Colour", "Farbe"}, {"Colour adjustment", "Farbanpassung"},
			{"Brightness", "Helligkeit"}, {"Contrast", "Kontrast"}, {"Saturation", "Saettigung"}, {"Reset colours", "Farben zuruecksetzen"},
			{"Post effects (vkBasalt)", "Effekte (vkBasalt)"}, {"Applies at the next start. Home key toggles the effects in game. Needs vkBasalt installed.", "Wirkt beim naechsten Spielstart. Pos1 schaltet die Effekte im Spiel an/aus. Benoetigt vkBasalt."},
			{"Volume", "Lautstaerke"}, {"Original sound chip (SPU2) at 48 kHz. Sound is paused while this menu is open.", "Original-Soundchip (SPU2) mit 48 kHz. Im Menue ist der Ton angehalten."},
			{"Gamepad", "Gamepad"}, {"Rumble", "Vibration"}, {"Rumble strength", "Vibrationsstaerke"},
			{"Any modern gamepad is detected automatically (Xbox, PlayStation, Switch, Deck).", "Jedes moderne Gamepad wird automatisch erkannt (Xbox, PlayStation, Switch, Deck)."},
			{"Mouse", "Maus"}, {"Mouse aiming", "Maus-Zielen"}, {"Sensitivity", "Empfindlichkeit"}, {"Zoom sensitivity", "Zoom-Empfindlichkeit"},
			{"Invert vertical", "Oben/unten vertauscht"}, {"Original crosshair while using the mouse (holds aim mode)", "Original-Fadenkreuz beim Maus-Spielen (Zielmodus halten)"},
			{"Click into the game to capture the mouse, ESC or F1 releases it.", "Klick ins Spiel faengt die Maus, ESC/F1 gibt sie wieder frei."},
			{"Key bindings", "Tastenbelegung"}, {"Click a field, then press a key or mouse button. Delete clears, ESC cancels.", "Feld anklicken, dann Taste oder Maustaste druecken. Entf leert, ESC bricht ab."},
			{"Action", "Aktion"}, {"Primary", "Taste 1"}, {"Secondary", "Taste 2"}, {"... press a key ...", "... Taste druecken ..."}, {"Reset to defaults", "Standardbelegung"},
			{"HD text (own re-typesetting for text pages without a pack)", "HD-Text (eigener Neusatz fuer Textseiten ohne Paket)"},
			{"Texture packs (PCSX2 format)", "Texturpakete (PCSX2-Format)"}, {"no packs found", "keine Pakete gefunden"},
			{"New packs: put the archive into the project's 'texturpakete' folder, %s imports it.", "Neue Pakete: Archiv in den Ordner 'texturpakete' des Projekts legen, %s importiert es."},
			{"Or extracted to %s/<name>/", "Oder entpackt nach %s/<Name>/"}, {"Apply and reload textures", "Uebernehmen und Texturen neu laden"}, {"Reload textures", "Texturen neu laden"},
			{"%u files active", "%u Dateien aktiv"}, {"Large packs are preloaded into video memory (short pause).", "Grosse Pakete werden vorab in den Grafikspeicher geladen (kurze Pause)."},
			{"A backup copy of the save data is made at every start.", "Bei jedem Start wird automatisch eine Sicherungskopie angelegt."},
			{"Memory card: not set", "Speicherkarte: nicht gesetzt"}, {"Memory card: ", "Speicherkarte: "}, {" (still empty)", " (noch leer)"}, {"\nLast saved: ", "\nZuletzt gespeichert: "},
			{"  Back to game  ", "  Weiterspielen  "}, {"  Quit game  ", "  Spiel beenden  "},
			{"ESC / F1 / Guide: menu   F7: free camera   F8: record video   F9: FPS   F10: turbo   F11 or Alt+Enter: fullscreen   F12: screenshot", "ESC / F1 / Guide: Menue   F7: freie Kamera   F8: Video aufnehmen   F9: FPS   F10: Turbo   F11 oder Alt+Enter: Vollbild   F12: Screenshot"},
			{"Menu: ESC / F1   |   Gamepad: Select + Start (or Guide)", "Menue: ESC / F1   |   Gamepad: Select + Start (oder Guide)"},
			// Online
			{"Play online", "Online spielen"}, {"Pick a server and connect. Then confirm \"TS2 Online\" in the game: Start Game hosts, Join Game lists the games on the server.", "Server waehlen und Verbinden. Dann im Spiel \"TS2 Online\" bestaetigen: Start Game hostet, Join Game zeigt die Spiele auf dem Server."},
			{"Player name", "Spielername"}, {"(none)", "(keiner)"}, {"Searching ...", "Suche ..."}, {"Refresh", "Aktualisieren"}, {"not full", "nicht voll"},
			{"no password", "ohne Passwort"}, {"favourites only", "nur Favoriten"}, {"Server", "Server"}, {"Players", "Spieler"}, {"Ping", "Ping"}, {"PW", "PW"},
			{"Message", "Nachricht"}, {" (no answer)", " (keine Antwort)"}, {"  (other version)", "  (andere Version)"}, {"yes", "ja"}, {"  Connect  ", "  Verbinden  "},
			{"address (host:port)", "Adresse (host:port)"}, {"Connect to address", "Direkt verbinden"}, {"Add favourite", "Als Favorit"},
			{"Host your own: ts2-server (see server/ in the project). Permanent listing: server/servers.txt.", "Eigenen Server hosten: ts2-server (siehe server/ im Projekt). Dauerhaft in die Liste: server/servers.txt."},
			{"Selected: ", "Ausgewaehlt: "}, {" - now confirm \"TS2 Online\" in the game", " - jetzt im Spiel \"TS2 Online\" bestaetigen"},
			{"Password", "Passwort"}, {"Password for %s", "Passwort fuer %s"}, {"Cancel", "Abbrechen"},
			// Aktionen
			{"Forward", "Vorwaerts"}, {"Backward", "Rueckwaerts"}, {"Strafe left", "Seitwaerts links"}, {"Strafe right", "Seitwaerts rechts"},
			{"Fire (R2)", "Feuer (R2)"}, {"Secondary fire (R1)", "Zweitfeuer (R1)"}, {"Zoom (hold)", "Zoom (halten)"}, {"Use / action (Cross)", "Benutzen / Aktion (Kreuz)"},
			{"Reload (Triangle)", "Nachladen (Dreieck)"}, {"Crouch (L1)", "Ducken (L1)"}, {"Aim mode (L2)", "Zielmodus (L2)"},
			{"Next weapon (D-pad right)", "Naechste Waffe (Steuerkreuz rechts)"}, {"Previous weapon (D-pad left)", "Vorige Waffe (Steuerkreuz links)"},
			{"Zoom in (D-pad up)", "Heranzoomen (Steuerkreuz hoch)"}, {"Zoom out (D-pad down)", "Wegzoomen (Steuerkreuz runter)"},
			{"Circle", "Kreis"}, {"Square", "Quadrat"}, {"Start (pause / game menu)", "Start (Pause / Spielmenue)"}, {"Select", "Select"},
			{"Look up (right stick)", "Umsehen hoch (rechter Stick)"}, {"Look down", "Umsehen runter"}, {"Look left", "Umsehen links"}, {"Look right", "Umsehen rechts"},
			// Tasten
			{"Mouse left", "Maus links"}, {"Mouse middle", "Maus Mitte"}, {"Mouse right", "Maus rechts"}, {"Wheel up", "Mausrad hoch"}, {"Wheel down", "Mausrad runter"},
			{"Mouse side 1", "Maus Seite 1"}, {"Mouse side 2", "Maus Seite 2"}, {"Mouse button {}", "Maustaste {}"}, {"Space", "Leertaste"}, {"Backspace", "Rueck"},
			{"Tab", "Tab"}, {"Enter", "Enter"}, {"Left", "Pfeil links"}, {"Up", "Pfeil hoch"}, {"Right", "Pfeil rechts"}, {"Down", "Pfeil runter"},
			{"Left Shift", "Shift links"}, {"Right Shift", "Shift rechts"}, {"Left Ctrl", "Strg links"}, {"Right Ctrl", "Strg rechts"}, {"Left Alt", "Alt links"}, {"Right Alt", "Alt rechts"},
			{"Caps Lock", "Feststell"}, {"Numpad Enter", "Enter (Ziffernblock)"}, {"Home", "Pos1"}, {"Page Up", "Bild hoch"}, {"Page Down", "Bild runter"}, {"End", "Ende"},
			{"Insert", "Einfg"}, {"Delete", "Entf"}, {"Esc", "Esc"}, {"Print", "Druck"}, {"Numpad +", "+ (Ziffernblock)"}, {"Numpad -", "- (Ziffernblock)"},
			{"Numpad *", "* (Ziffernblock)"}, {"Numpad /", "/ (Ziffernblock)"}, {"Numpad ,", ", (Ziffernblock)"}, {"Numpad {}", "{} (Ziffernblock)"}, {"Key 0x{:x}", "Taste 0x{:x}"},
		};
		const auto it = de.find(en);
		return it == de.end() ? en : it->second;
	}
	// ---- Freie Belegung: Aktionen und ihre Tasten (Tastatur = KeySym, Maus = kMaus + Knopf) ----
	constexpr int kMaus = 0x10000000;  // + 1 links, 2 Mitte, 3 rechts, 4 Rad hoch, 5 Rad runter, 8/9 Seitentasten
	enum Wirkung : int { W_TASTE = 0, W_STICK = 1, W_ZOOM = 2 };
	struct Aktion { const char* name; int wirkung; unsigned wert; int std1, std2; };
	// PS2-Bits wie libpad (1 = gedrueckt): SEL 0x1, L3 0x2, R3 0x4, START 0x8, HOCH 0x10, RECHTS 0x20, RUNTER 0x40,
	// LINKS 0x80, L2 0x100, R2 0x200, L1 0x400, R1 0x800, DREIECK 0x1000, KREIS 0x2000, KREUZ 0x4000, QUADRAT 0x8000.
	// Stick-Bits: 0-3 linker Stick (hoch runter links rechts), 4-7 rechter Stick. Standard = TS2-PS2-Belegung fuer Maus+Tastatur.
	static const Aktion kAktionen[] = {
		{"Forward", W_STICK, 1, 'w', 0}, {"Backward", W_STICK, 2, 's', 0},
		{"Strafe left", W_STICK, 4, 'a', 0}, {"Strafe right", W_STICK, 8, 'd', 0},
		{"Fire (R2)", W_TASTE, 0x0200, kMaus + 1, 0}, {"Secondary fire (R1)", W_TASTE, 0x0800, kMaus + 2, 'q'},
		{"Zoom (hold)", W_ZOOM, 0, kMaus + 3, 0},
		{"Use / action (Cross)", W_TASTE, 0x4000, 'e', ' '},
		{"Reload (Triangle)", W_TASTE, 0x1000, 'r', 0},
		{"Crouch (L1)", W_TASTE, 0x0400, 'c', 0xffe3},
		{"Aim mode (L2)", W_TASTE, 0x0100, 0xffe1, 0},
		{"Next weapon (D-pad right)", W_TASTE, 0x0020, kMaus + 4, 0xff53}, {"Previous weapon (D-pad left)", W_TASTE, 0x0080, kMaus + 5, 0xff51},
		{"Zoom in (D-pad up)", W_TASTE, 0x0010, 0xff52, 0}, {"Zoom out (D-pad down)", W_TASTE, 0x0040, 0xff54, 0},
		{"Circle", W_TASTE, 0x2000, 'x', 0xff08}, {"Square", W_TASTE, 0x8000, 'f', 0},
		{"Start (pause / game menu)", W_TASTE, 0x0008, 0xff0d, 'p'}, {"Select", W_TASTE, 0x0001, 0xff09, 0},
		{"L3", W_TASTE, 0x0002, 0xffe9, 0}, {"R3", W_TASTE, 0x0004, 'g', 0},
		{"Look up (right stick)", W_STICK, 16, 'i', 0}, {"Look down", W_STICK, 32, 'k', 0},
		{"Look left", W_STICK, 64, 'j', 0}, {"Look right", W_STICK, 128, 'l', 0},
	};
	constexpr int kBelegungVersion = 2; // bei neuer Standardbelegung erhoehen: alte gespeicherte Belegung wird dann ersetzt
	constexpr int kAnzAktionen = static_cast<int>(sizeof(kAktionen) / sizeof(kAktionen[0]));
	inline std::string tastenName(int c)
	{
		if (c == 0) return "-";
		if (c >= kMaus)
		{
			switch (c - kMaus) { case 1: return tr("Mouse left"); case 2: return tr("Mouse middle"); case 3: return tr("Mouse right");
				case 4: return tr("Wheel up"); case 5: return tr("Wheel down"); case 8: return tr("Mouse side 1"); case 9: return tr("Mouse side 2");
				default: return fmt::format(fmt::runtime(tr("Mouse button {}")), c - kMaus); }
		}
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return std::string(1, static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
		switch (c)
		{
			case ' ': return tr("Space"); case 0xff08: return tr("Backspace"); case 0xff09: return tr("Tab"); case 0xff0d: return tr("Enter");
			case 0xff51: return tr("Left"); case 0xff52: return tr("Up"); case 0xff53: return tr("Right"); case 0xff54: return tr("Down");
			case 0xffe1: return tr("Left Shift"); case 0xffe2: return tr("Right Shift"); case 0xffe3: return tr("Left Ctrl"); case 0xffe4: return tr("Right Ctrl");
			case 0xffe9: return tr("Left Alt"); case 0xffea: return tr("Right Alt"); case 0xffe5: return tr("Caps Lock"); case 0xff8d: return tr("Numpad Enter");
			case 0xff50: return tr("Home"); case 0xff55: return tr("Page Up"); case 0xff56: return tr("Page Down"); case 0xff57: return tr("End");
			case 0xff63: return tr("Insert"); case 0xffff: return tr("Delete"); case 0xff1b: return tr("Esc"); case 0xff61: return tr("Print");
			case 0xffab: return tr("Numpad +"); case 0xffad: return tr("Numpad -"); case 0xffaa: return tr("Numpad *");
			case 0xffaf: return tr("Numpad /"); case 0xffae: return tr("Numpad ,");
			default: break;
		}
		if (c >= 0xffbe && c <= 0xffc9) return fmt::format("F{}", c - 0xffbe + 1);
		if (c >= 0xffb0 && c <= 0xffb9) return fmt::format(fmt::runtime(tr("Numpad {}")), c - 0xffb0);
		if (c >= 0x21 && c < 0x7f) return std::string(1, static_cast<char>(c));
		if (c >= 0xa1 && c < 0x100) // Latin-1 (Umlaute, ß usw.) als UTF-8
		{
			std::string u; u += static_cast<char>(0xC0 | (c >> 6)); u += static_cast<char>(0x80 | (c & 0x3F)); return u;
		}
		return fmt::format(fmt::runtime(tr("Key 0x{:x}")), c);
	}

	struct Cfg
	{
		int upscale = 4, aspect = 0, cas = 1, casSharp = 25, tv = 0, filter = 2, aniso = 16, dither = 0;
		int sbB = 50, sbC = 50, sbS = 50, rumblePct = 100, hz = 50, volume = 100, camBlur = 1, nachzieh = 0;
		bool fxaa = false, shadeboost = false, fullscreen = false, showFps = false, rumble = true, vsync = false, startMenu = true;
		int tri = 2;             // Trilineare Filterung: 0 automatisch (PCSX2-Standard), 1 wie PS2, 2 erzwungen (nutzt Mipmaps der Ersatztexturen)
		int fx = 0;              // Effekte (vkBasalt): 0 aus, 1 original-plus, 2 klar, 3 filmisch - wirkt beim naechsten Start
		bool hdText = true;      // Neusatz der Textseiten (TeX Gyre), hat Vorrang vor Paketen
		bool showInput = false;  // Eingabe-Anzeige oben links (Debug), getrennt von der FPS-Anzeige
		int lang = -1;           // Menuesprache: -1 automatisch, 0 English, 1 Deutsch
		bool maus = !ps2xAufSteamDeck(); // Mauszielen: Klick ins Spielfenster faengt die Maus (ESC/F1 gibt sie frei); Steam Deck: aus
		int mausSens = 30;       // Empfindlichkeit in Zehntel (1.0-10.0)
		bool mausInvert = false; // Maus: oben/unten vertauscht
		bool mausZielmodus = true; // Mauszielen: Zielmodus (Original-Fadenkreuz) automatisch halten
		int zoomSens = 50;       // Empfindlichkeit beim Zoomen in % der normalen
		int bel[32][2] = {};     // Belegung je Aktion (2 Tasten), Standard in kAktionen
		int belVersion = 0;      // Version der gespeicherten Belegung (0 = keine gespeichert)
		Cfg() { for (int i = 0; i < kAnzAktionen; ++i) { bel[i][0] = kAktionen[i].std1; bel[i][1] = kAktionen[i].std2; } belVersion = kBelegungVersion; }
		std::string onlineServer, onlineName, onlinePw, onlineFavs; // Online (TS2): gewaehlter Server, Spielername, Passwort, Favoriten (Komma)
		std::string packs = "*"; // aktive Texturpakete ("*" = alle vorhandenen, Voreinstellung) (Ordnernamen in texturquellen/, durch Komma, Reihenfolge = Prioritaet)
	};
	std::mutex m;
	Cfg cfg;
	std::atomic<double> applyAt{0.0}; // Zeitpunkt (ImGui-Zeit), ab dem geaenderte Einstellungen angewendet werden

	std::string dir()
	{
#ifdef _WIN32
		const char* xdg = std::getenv("APPDATA");
		const char* home = std::getenv("USERPROFILE");
#else
		const char* xdg = std::getenv("XDG_CONFIG_HOME");
		const char* home = std::getenv("HOME");
#endif
		std::string d = (xdg && *xdg) ? std::string(xdg) : std::string(home ? home : "/tmp") + "/.config";
		d += "/"; d += cfgName();
		ps2x_mkdir(d.c_str(), 0755);
		return d;
	}
	std::string iniPath()
	{
		// PS2XGS_CFG=<Datei>: andere Einstellungsdatei (Vergleichsbilder in Testlaeufen)
		if (const char* c = std::getenv("PS2XGS_CFG"); c && *c)
			return c;
		return dir() + "/einstellungen.ini";
	}

	void belegungPruefen();
	void load(float envUpscale)
	{
		std::lock_guard<std::mutex> lk(m);
		(void)envUpscale; // Standard: Remaster-Preset (4x); ein gespeicherter Wert hat Vorrang
#ifdef __linux__
		{
			// Steam Deck (oder anderes Valve-Geraet): schwaechere GPU -> 2x intern (1280x800-Bildschirm), FXAA aus
			std::ifstream dv("/sys/devices/virtual/dmi/id/board_vendor");
			std::string vend; std::getline(dv, vend);
			const char* sd = std::getenv("SteamDeck");
			if (vend.rfind("Valve", 0) == 0 || (sd && *sd == '1'))
			{
				cfg.upscale = 2; cfg.fxaa = false;
				Console.WriteLn("[ps2xmenu] Steam Deck erkannt: Standard 2x intern, FXAA aus (Einstellungsdatei hat Vorrang)");
			}
		}
#endif
		cfg.belVersion = 0; // steht die Version nicht in der Datei, gilt eine dort gespeicherte Belegung als veraltet
		std::ifstream f(iniPath());
		std::string line;
		while (std::getline(f, line))
		{
			const auto eq = line.find('=');
			if (eq == std::string::npos || line[0] == '#')
				continue;
			const std::string k = line.substr(0, eq);
			const int v = std::atoi(line.c_str() + eq + 1);
			if (k == "aufloesung") cfg.upscale = std::clamp(v, 1, 8);
			else if (k == "bildformat") cfg.aspect = std::clamp(v, 0, 3);
			else if (k == "fxaa") cfg.fxaa = v != 0;
			else if (k == "cas") cfg.cas = std::clamp(v, 0, 2);
			else if (k == "cas_staerke") cfg.casSharp = std::clamp(v, 0, 100);
			else if (k == "dithering") cfg.dither = std::clamp(v, 0, 2);
			else if (k == "tv_shader") cfg.tv = std::clamp(v, 0, 7);
			else if (k == "farbe") cfg.shadeboost = v != 0;
			else if (k == "helligkeit") cfg.sbB = std::clamp(v, 1, 100);
			else if (k == "kontrast") cfg.sbC = std::clamp(v, 1, 100);
			else if (k == "saettigung") cfg.sbS = std::clamp(v, 1, 100);
			else if (k == "texturfilter") cfg.filter = std::clamp(v, 0, 3);
			else if (k == "anisotrop") cfg.aniso = std::clamp(v, 0, 16);
			else if (k == "vollbild") cfg.fullscreen = v != 0;
			else if (k == "fps_anzeige") cfg.showFps = v != 0;
			else if (k == "vibration") cfg.rumble = v != 0;
			else if (k == "vibration_staerke") cfg.rumblePct = std::clamp(v, 0, 100);
			else if (k == "vsync") cfg.vsync = v != 0;
			else if (k == "startmenue") cfg.startMenu = v != 0;
			else if (k == "bildrate") cfg.hz = (v == 60 && std::getenv("SOTC_60HZ_EXPERIMENT")) ? 60 : 50; // 60 Hz: Figur-Physik noch fehlerhaft
			else if (k == "lautstaerke") cfg.volume = std::clamp(v, 0, 200);
			else if (k == "kamera_unschaerfe") cfg.camBlur = v != 0 ? 1 : 0;
			else if (k == "nachzieheffekt") cfg.nachzieh = v != 0 ? 1 : 0;
			else if (k == "hd_text") cfg.hdText = v != 0;
			else if (k == "maus_zielen") cfg.maus = v != 0;
			else if (k == "maus_empfindlichkeit") cfg.mausSens = std::clamp(v, 1, 100);
			else if (k == "maus_invertiert") cfg.mausInvert = v != 0;
			else if (k == "maus_zielmodus") cfg.mausZielmodus = v != 0;
			else if (k == "sprache") cfg.lang = std::clamp(v, -1, 1);
			else if (k == "eingabeanzeige") cfg.showInput = v != 0;
			else if (k == "zoom_empfindlichkeit") cfg.zoomSens = std::clamp(v, 5, 200);
			else if (k == "belegung_version") cfg.belVersion = v;
			else if (k.rfind("taste_", 0) == 0)
			{
				const int id = std::atoi(k.c_str() + 6);
				if (id >= 0 && id < kAnzAktionen)
				{
					const std::string val = line.substr(eq + 1);
					const auto komma = val.find(',');
					cfg.bel[id][0] = static_cast<int>(std::strtol(val.c_str(), nullptr, 10));
					cfg.bel[id][1] = komma == std::string::npos ? 0 : static_cast<int>(std::strtol(val.c_str() + komma + 1, nullptr, 10));
				}
			}
			else if (k == "trilinear") cfg.tri = std::clamp(v, 0, 2);
			else if (k == "effekte") { const std::string val = line.substr(eq + 1); cfg.fx = val == "original-plus" ? 1 : val == "klar" ? 2 : val == "filmisch" ? 3 : 0; }
			else if (k == "texturpakete") cfg.packs = line.substr(eq + 1);
			else if (k == "online_server") cfg.onlineServer = line.substr(eq + 1);
			else if (k == "online_name") cfg.onlineName = line.substr(eq + 1);
			else if (k == "online_passwort") cfg.onlinePw = line.substr(eq + 1);
			else if (k == "online_favoriten") cfg.onlineFavs = line.substr(eq + 1);
		}
		belegungPruefen();
	}
	void belegungPruefen()
	{
		// Ohne Sperre aufrufen (load haelt sie): aeltere/fehlende Belegung durch die aktuelle Standardbelegung ersetzen
		if (cfg.belVersion < kBelegungVersion)
		{
			for (int i = 0; i < kAnzAktionen; ++i) { cfg.bel[i][0] = kAktionen[i].std1; cfg.bel[i][1] = kAktionen[i].std2; }
			cfg.belVersion = kBelegungVersion;
		}
	}
	void save()
	{
		Cfg c;
		{ std::lock_guard<std::mutex> lk(m); c = cfg; }
		std::ofstream f(iniPath(), std::ios::trunc);
		f << "# " << cfgName() << " - Einstellungen (werden im Spielmenue geaendert, ESC/F1 oder Guide-Taste)\n"
		  << "aufloesung=" << c.upscale << "\nbildformat=" << c.aspect << "\nfxaa=" << c.fxaa << "\ncas=" << c.cas
		  << "\ncas_staerke=" << c.casSharp << "\ndithering=" << c.dither << "\ntv_shader=" << c.tv << "\nfarbe=" << c.shadeboost << "\nhelligkeit=" << c.sbB
		  << "\nkontrast=" << c.sbC << "\nsaettigung=" << c.sbS << "\ntexturfilter=" << c.filter << "\nanisotrop=" << c.aniso
		  << "\nvollbild=" << c.fullscreen << "\nfps_anzeige=" << c.showFps << "\nvibration=" << c.rumble
		  << "\nvibration_staerke=" << c.rumblePct << "\nvsync=" << c.vsync << "\nbildrate=" << c.hz << "\nlautstaerke=" << c.volume << "\nkamera_unschaerfe=" << c.camBlur << "\nnachzieheffekt=" << c.nachzieh << "\nstartmenue=" << c.startMenu << "\neffekte=" << (c.fx == 1 ? "original-plus" : c.fx == 2 ? "klar" : c.fx == 3 ? "filmisch" : "aus") << "\ntrilinear=" << c.tri << "\nhd_text=" << c.hdText << "\nmaus_zielen=" << c.maus << "\nmaus_empfindlichkeit=" << c.mausSens << "\nmaus_invertiert=" << c.mausInvert << "\nmaus_zielmodus=" << c.mausZielmodus << "\nsprache=" << c.lang << "\neingabeanzeige=" << c.showInput << "\nbelegung_version=" << c.belVersion << "\nzoom_empfindlichkeit=" << c.zoomSens << "\ntexturpakete=" << c.packs << "\nonline_server=" << c.onlineServer << "\nonline_name=" << c.onlineName << "\nonline_passwort=" << c.onlinePw << "\nonline_favoriten=" << c.onlineFavs << "\n";
		for (int i = 0; i < kAnzAktionen; ++i)
			f << "taste_" << i << "=" << c.bel[i][0] << "," << c.bel[i][1] << "   # " << kAktionen[i].name << "\n";
	}
	// ---------- Texturquellen (HD-Text + Pakete) -> zusammengesetzter Ersatzordner ----------
	// Quellen: <dir>/texturquellen/<name>/... (hd-text = unser Neusatz; Pakete werden von "SotC starten" aus
	// texturpakete/*.zip|7z|rar importiert). Ziel: <Textures>/SCES-53326/replacements als Verweise (Symlinks); spaetere
	// Quelle ueberschreibt fruehere, HD-Text zuletzt. PS2XGS_TEXDIR (Testlaeufe) schaltet die Verwaltung ab.
	// PS2XGS_TEXQUELLEN: Ordner mit den Texturquellen (Startskript: <Projekt>/build/texturquellen), sonst ~/.config/...
	std::string quellenDir()
	{
		if (const char* q = std::getenv("PS2XGS_TEXQUELLEN"); q && *q) return q;
		return dir() + "/texturquellen";
	}
	std::string texRoot()
	{
		if (const char* t = std::getenv("PS2XGS_TEXDIR"); t && *t) return t;
		return dir() + "/texturen";
	}
	bool texManaged() { const char* t = std::getenv("PS2XGS_TEXDIR"); return !(t && *t); }
	std::vector<std::string> listSources()
	{
		std::vector<std::string> out;
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(quellenDir(), ec))
			if (e.is_directory(ec) && e.path().filename() != "hd-text" && e.path().filename().string()[0] != '.')
				out.push_back(e.path().filename().string());
		std::sort(out.begin(), out.end());
		return out;
	}
	std::vector<std::string> listSources();
	std::vector<std::string> activePacks(const std::string& p)
	{
		if (p == "*") return listSources();
		std::vector<std::string> out; std::string cur;
		for (char ch : p + ",") { if (ch == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); } else cur += ch; }
		return out;
	}
	void setPackActive(std::string& packs, const std::string& name, bool on)
	{
		auto v = activePacks(packs);
		v.erase(std::remove(v.begin(), v.end(), name), v.end());
		if (on) v.push_back(name);
		std::string j; for (const auto& x : v) { if (!j.empty()) j += ','; j += x; }
		packs = j;
	}
	bool sameCfg(const Cfg& a, const Cfg& b)
	{
		return a.upscale == b.upscale && a.aspect == b.aspect && a.cas == b.cas && a.casSharp == b.casSharp && a.tv == b.tv && a.filter == b.filter &&
		       a.aniso == b.aniso && a.dither == b.dither && a.sbB == b.sbB && a.sbC == b.sbC && a.sbS == b.sbS && a.rumblePct == b.rumblePct &&
		       a.hz == b.hz && a.volume == b.volume && a.camBlur == b.camBlur && a.nachzieh == b.nachzieh && a.fxaa == b.fxaa && a.shadeboost == b.shadeboost &&
		       a.fullscreen == b.fullscreen && a.showFps == b.showFps && a.rumble == b.rumble && a.vsync == b.vsync && a.startMenu == b.startMenu &&
		       a.hdText == b.hdText && a.maus == b.maus && a.mausSens == b.mausSens && a.mausInvert == b.mausInvert && a.mausZielmodus == b.mausZielmodus && a.lang == b.lang && a.showInput == b.showInput &&
		       a.zoomSens == b.zoomSens && std::memcmp(a.bel, b.bel, sizeof(a.bel)) == 0 && a.packs == b.packs && a.fx == b.fx && a.tri == b.tri &&
		       a.onlineServer == b.onlineServer && a.onlineName == b.onlineName && a.onlinePw == b.onlinePw && a.onlineFavs == b.onlineFavs;
	}
	// Schluessel wie PCSX2 ihn beim Laden bildet: "<TEX0-Hash>-<CLUT-Hash>-<Bits>" ohne das alte TCC-Bit (0x4000),
	// das aeltere Pakete noch im Namen tragen (RemoveUnusedBits). Gleiche Schluessel = dieselbe Textur.
	static std::string texKey(const std::string& file)
	{
		const auto dot = file.find('.');
		std::string base = file.substr(0, dot);
		const auto d = base.rfind('-');
		if (d == std::string::npos) return base;
		unsigned long bits = std::strtoul(base.c_str() + d + 1, nullptr, 16) & ~0x4000ul;
		char b[16]; std::snprintf(b, sizeof(b), "%08lx", bits);
		return base.substr(0, d + 1) + b;
	}
	// keys: bereits belegte Texturen (Pakete zuerst); eine Quelle mit nurNeue=true fuellt nur Luecken.
	static void linkTree(const std::filesystem::path& src, const std::filesystem::path& dst, unsigned& n,
		std::set<std::string>& keys, bool nurNeue = false)
	{
		std::error_code ec;
		// <quelle>/.ausschluss.txt: Dateinamen, die nicht ersetzt werden (z. B. Palettentexturen, deren Ersatz PCSX2 in
		// den langsamen Pfad zwingt: jede Palettenvariante einzeln umgewandelt -> Draw-Calls x3..x8)
		std::set<std::string> aus;
		{
			std::ifstream f((src / ".ausschluss.txt").string());
			std::string l;
			while (std::getline(f, l))
				if (!l.empty() && l[0] != '#') { while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back(); aus.insert(l); }
		}
		for (const auto& e : std::filesystem::recursive_directory_iterator(src, std::filesystem::directory_options::follow_directory_symlink, ec))
		{
			if (!e.is_regular_file(ec)) continue;
			std::string ext = e.path().extension().string();
			std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
			if (ext != ".png" && ext != ".dds") continue;
			if (aus.count(e.path().filename().string())) continue;
			const std::string key = texKey(e.path().filename().string());
			if (nurNeue && keys.count(key)) continue;
			keys.insert(key);
			const auto to = dst / e.path().filename();
			// Alpha-Zusatzdatei (tools/dds_alpha.py) wird ueber den echten Pfad gelesen, kein eigener Verweis noetig
			std::filesystem::remove(to, ec);
			std::filesystem::create_symlink(std::filesystem::absolute(e.path()), to, ec);
			if (ec) { ec.clear(); std::filesystem::create_hard_link(e.path(), to, ec); }
			if (ec) { ec.clear(); std::filesystem::copy_file(e.path(), to, std::filesystem::copy_options::overwrite_existing, ec); }
			if (!ec) ++n;
		}
	}
	// Zusammensetzen; Rueckgabe = Anzahl Dateien. Darf vor dem GS-Start und auf dem GS-Thread laufen.
	unsigned assembleTextures()
	{
		if (!texManaged()) return 0;
		std::error_code ec;
		const std::filesystem::path root = texRoot();
		const auto dst = root / envOr("PS2XGS_SERIAL", "SCES-53326") / "replacements"; // PCSX2-Ordnername = Seriennummer
		const auto marker = dst / ".verwaltet";
		std::filesystem::create_directories(dst, ec);
		if (!std::filesystem::exists(marker, ec))
		{
			// Altbestand (von Hand oder von frueheren Startskripten kopiert) einmalig als Quelle "alt" sichern
			const auto alt = std::filesystem::path(quellenDir()) / "alt" / "replacements";
			bool any = false;
			for (const auto& e : std::filesystem::directory_iterator(dst, ec))
				if (e.is_regular_file(ec) && !e.is_symlink(ec)) { if (!any) { std::filesystem::create_directories(alt, ec); any = true; } std::filesystem::rename(e.path(), alt / e.path().filename(), ec); }
			std::ofstream(marker.string()) << "Dieser Ordner wird vom Spielmenue (Texturen) verwaltet - Inhalt sind Verweise auf texturquellen/.\n";
		}
		for (const auto& e : std::filesystem::directory_iterator(dst, ec))
			if (e.is_symlink(ec)) std::filesystem::remove(e.path(), ec);
		unsigned n = 0;
		const auto q = std::filesystem::path(quellenDir());
		std::string pk; bool hd; { std::lock_guard<std::mutex> lk(m); pk = cfg.packs; hd = cfg.hdText; }
		// Pakete zuerst: bringen sie eine Textseite selbst mit (z. B. Origami: alle Sprachen, Originalschrift),
		// gewinnt das Paket; unser Neusatz fuellt nur Seiten, die kein aktives Paket abdeckt.
		std::set<std::string> keys;
		for (const auto& name : activePacks(pk)) linkTree(q / name, dst, n, keys);
		if (hd) linkTree(q / "hd-text", dst, n, keys, true);
		return n;
	}
	// in die PCSX2-Einstellungen uebertragen (CPU-Thread bzw. vor dem Start)
	void toSettings(SettingsInterface& si)
	{
		Cfg c;
		{ std::lock_guard<std::mutex> lk(m); c = cfg; }
		// Bildformat: 0 = 16:9 (Breitbild-Modus des Spiels erzwungen), 1 = wie Spieloption, 2 = 4:3, 3 = Fenster fuellen
		const bool wide = (c.aspect == 0) || (c.aspect == 1 && gameWide.load());
		si.SetFloatValue("EmuCore/GS", "upscale_multiplier", static_cast<float>(c.upscale));
		si.SetStringValue("EmuCore/GS", "VideoCaptureCodec", envOr("PS2XGS_CAPTURE_CODEC", "libx264"));
		si.SetIntValue("EmuCore/GS", "VideoCaptureBitrate", std::atoi(envOr("PS2XGS_CAPTURE_KBPS", "40000")));
		// x264: konstante Qualitaet statt Bitrate, schneller Preset (Echtzeit bei 4K/60 auf vielen Kernen); PS2XGS_CAPTURE_PARAMS ueberschreibt
		si.SetBoolValue("EmuCore/GS", "EnableVideoCaptureParameters", true);
		si.SetStringValue("EmuCore/GS", "VideoCaptureParameters", envOr("PS2XGS_CAPTURE_PARAMS", "crf=16:preset=veryfast"));
		si.SetStringValue("EmuCore/GS", "CaptureContainer", "mp4");
		si.SetBoolValue("EmuCore/GS", "EnableVideoCapture", true);
		// Ton mit aufnehmen (PS2XGS_CAPTURE_AUDIO=0 fuer Testlaeufe ohne Tonausgabe: sonst wartet das Video auf Tonpakete)
		si.SetBoolValue("EmuCore/GS", "EnableAudioCapture", std::atoi(envOr("PS2XGS_CAPTURE_AUDIO", "1")) != 0);
		si.SetIntValue("EmuCore/GS", "AudioCaptureBitrate", 192);
		si.SetStringValue("EmuCore/GS", "AudioCaptureCodec", "aac");
		si.SetStringValue("EmuCore/GS", "AspectRatio", c.aspect == 3 ? "Stretch" : (wide ? "16:9" : "4:3"));
		si.SetBoolValue("EmuCore/GS", "fxaa", c.fxaa);
		si.SetIntValue("EmuCore/GS", "CASMode", c.cas);
		si.SetIntValue("EmuCore/GS", "CASSharpness", c.casSharp);
		// PS2-Dithering: hochskaliert wird es zum feinen Gittermuster auf Gesichtern/Waenden -> im Remaster aus
		si.SetIntValue("EmuCore/GS", "dithering_ps2", c.dither);
		// Oberste/unterste Bildzeile enthaelt Restdaten (bunte Pixel am Bildrand) -> je 2 Zeilen abschneiden
		si.SetIntValue("EmuCore/GS", "CropTop", 2);
		si.SetIntValue("EmuCore/GS", "CropBottom", 2);
		si.SetIntValue("EmuCore/GS", "TVShader", c.tv);
		si.SetBoolValue("EmuCore/GS", "ShadeBoost", c.shadeboost);
		si.SetIntValue("EmuCore/GS", "ShadeBoost_Brightness", c.sbB);
		si.SetIntValue("EmuCore/GS", "ShadeBoost_Contrast", c.sbC);
		si.SetIntValue("EmuCore/GS", "ShadeBoost_Saturation", c.sbS);
		si.SetIntValue("EmuCore/GS", "filter", c.filter);
		si.SetIntValue("EmuCore/GS", "MaxAnisotropy", c.aniso);
		si.SetIntValue("EmuCore/GS", "TriFilter", c.tri == 0 ? -1 : c.tri == 1 ? 1 : 2); // TriFiltering: Automatic/PS2/Forced
		si.SetBoolValue("EmuCore/GS", "OsdShowFPS", c.showFps);
		si.SetIntValue("EmuCore/GS", "OsdPerformancePos", c.showFps ? 3 : 0);
		si.SetIntValue("EmuCore/GS", "VsyncEnable", c.vsync ? 1 : 0);
		si.SetBoolValue("EmuCore/GS", "hw_mipmap", true);           // Mipmaps in Hardware: kein Flimmern in der Ferne
		si.SetIntValue("EmuCore/GS", "linear_present_mode", 1);     // weiches Herunterrechnen = Supersampling-Effekt
	}

	// ---------- Zeichnen (GS-Thread) ----------
	u32 s_prevNav = 0;
	void feedInput()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
		io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
		const u32 n = nav.load();
		const u32 ch = n ^ s_prevNav;
		static const ImGuiKey kKeys[6] = {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
			ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight};
		for (int i = 0; i < 6; ++i)
			if (ch & (1u << i))
				io.AddKeyEvent(kKeys[i], (n >> i) & 1u);
		s_prevNav = n;
		const int mx = mouseX.load(), my = mouseY.load();
		if (mx >= 0)
			io.AddMousePosEvent(static_cast<float>(mx), static_cast<float>(my));
		static u32 s_prevMb = 0;
		const u32 mb = mouseBtn.load();
		for (int i = 0; i < 3; ++i)
			if ((mb ^ s_prevMb) & (1u << i))
				io.AddMouseButtonEvent(i, (mb >> i) & 1u);
		s_prevMb = mb;
		if (const int w = wheel.exchange(0))
			io.AddMouseWheelEvent(0.0f, static_cast<float>(w));
	}

	void style()
	{
		// TimeSplitters 2: tiefes Blau-Schwarz, cyanfarbene Linien, gruene Hervorhebung, weisse Schrift - wie die Spielmenues
		ImGuiStyle& st = ImGui::GetStyle();
		st.WindowRounding = 4.0f; st.FrameRounding = 3.0f; st.GrabRounding = 3.0f; st.TabRounding = 3.0f; st.PopupRounding = 4.0f; st.ScrollbarRounding = 3.0f;
		st.WindowBorderSize = 1.0f; st.FrameBorderSize = 0.0f; st.TabBorderSize = 0.0f;
		st.WindowPadding = ImVec2(26, 20); st.FramePadding = ImVec2(10, 6); st.ItemSpacing = ImVec2(10, 8); st.ItemInnerSpacing = ImVec2(8, 6); st.CellPadding = ImVec2(8, 5);
		ImVec4* c = st.Colors;
		const ImVec4 cyan(0.42f, 0.86f, 1.00f, 1.0f), green(0.50f, 1.00f, 0.42f, 1.0f);
		c[ImGuiCol_WindowBg] = ImVec4(0.025f, 0.045f, 0.10f, 0.95f);
		c[ImGuiCol_PopupBg] = ImVec4(0.03f, 0.05f, 0.11f, 0.98f);
		c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
		c[ImGuiCol_Border] = ImVec4(cyan.x, cyan.y, cyan.z, 0.45f);
		c[ImGuiCol_Text] = ImVec4(0.94f, 0.97f, 1.00f, 1.0f);
		c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.66f, 0.80f, 1.0f);
		c[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.14f, 0.27f, 0.92f);
		c[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.24f, 0.43f, 0.95f);
		c[ImGuiCol_FrameBgActive] = ImVec4(0.17f, 0.32f, 0.56f, 1.0f);
		c[ImGuiCol_Button] = ImVec4(0.10f, 0.20f, 0.37f, 0.95f);
		c[ImGuiCol_ButtonHovered] = ImVec4(0.17f, 0.42f, 0.68f, 1.0f);
		c[ImGuiCol_ButtonActive] = ImVec4(0.24f, 0.62f, 0.92f, 1.0f);
		c[ImGuiCol_Header] = ImVec4(0.13f, 0.28f, 0.50f, 0.90f);
		c[ImGuiCol_HeaderHovered] = ImVec4(0.19f, 0.44f, 0.72f, 0.95f);
		c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.60f, 0.92f, 1.0f);
		c[ImGuiCol_Tab] = ImVec4(0.06f, 0.11f, 0.22f, 1.0f);
		c[ImGuiCol_TabHovered] = ImVec4(0.19f, 0.44f, 0.72f, 1.0f);
		c[ImGuiCol_TabSelected] = ImVec4(0.15f, 0.36f, 0.64f, 1.0f);
		c[ImGuiCol_TabSelectedOverline] = cyan;
		c[ImGuiCol_TabDimmed] = c[ImGuiCol_Tab];
		c[ImGuiCol_TabDimmedSelected] = c[ImGuiCol_TabSelected];
		c[ImGuiCol_SliderGrab] = cyan;
		c[ImGuiCol_SliderGrabActive] = ImVec4(0.65f, 1.0f, 1.0f, 1.0f);
		c[ImGuiCol_CheckMark] = green;
		c[ImGuiCol_NavCursor] = green;
		c[ImGuiCol_Separator] = ImVec4(cyan.x, cyan.y, cyan.z, 0.35f);
		c[ImGuiCol_TableHeaderBg] = ImVec4(0.09f, 0.17f, 0.32f, 1.0f);
		c[ImGuiCol_TableBorderLight] = ImVec4(cyan.x, cyan.y, cyan.z, 0.15f);
		c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
		c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0.2f);
		c[ImGuiCol_ScrollbarGrab] = ImVec4(0.17f, 0.42f, 0.68f, 0.8f);
	}

	std::string memcardInfo()
	{
		const char* mc = std::getenv("SOTC_MEMCARD");
		if (!mc || !*mc)
			return tr("Memory card: not set");
		struct stat sb{};
		if (stat(mc, &sb) != 0)
			return std::string(tr("Memory card: ")) + mc + tr(" (still empty)");
		char tbuf[64];
		std::strftime(tbuf, sizeof(tbuf), "%d.%m.%Y %H:%M", std::localtime(&sb.st_mtime));
		return std::string(tr("Memory card: ")) + mc + tr("\nLast saved: ") + tbuf;
	}

	bool combo(const char* label, int* v, const char* const* items, int n)
	{
		return ImGui::Combo(label, v, items, n);
	}

static bool s_onlineTabWaehlen = false;
	// ---------- Online: Serverbrowser (TS2-Online-Mehrspieler, siehe server/ im Port-Repo) ----------
	// Liste: Community-Serverliste (GitHub, PS2XGS_SERVERLISTE) + eigene Favoriten. Jeder Server beantwortet eine kurze
	// UDP-Abfrage "T2IN"+Nonce mit Name, Spielern, Passwort-Kennung und Nachricht; daraus auch der Ping.
	// Die Spiel-Seite (Laufzeit) liest online_server/online_name/online_passwort aus der Einstellungsdatei beim
	// Netzwerkstart (Arcade -> Network). PS2XGS_ONLINE=1 (Startskript von TS2) schaltet den Reiter ein.
	namespace online
	{
		struct Eintrag
		{
			std::string adresse, name, motd;
			int spieler = -1, max = 0, ping = -1, proto = 0;
			bool pw = false, fav = false, antwort = false;
		};
		std::mutex om;
		std::vector<Eintrag> liste;
		std::atomic<bool> laeuft{false};
		std::string meldung = "Noch nicht aktualisiert";
		std::string spielStatus;            // von der Laufzeit (ps2xgs_online_status)
		std::atomic<bool> oeffnen{false};   // Laufzeit: Netzwerk-Konfiguration im Spiel erreicht -> Browser zeigen
		bool vonSpielGeoeffnet = false;
		int auswahl = -1;
		char eingabe[128] = "";
		char pwEingabe[64] = "";
		char nameEingabe[32] = "";
		std::string pwFuer;
		bool nurFrei = false, nurOffen = false, nurFav = false;

		bool aktiv() { const char* e = std::getenv("PS2XGS_ONLINE"); return e && *e == '1'; }
		const char* listenUrl() { return envOr("PS2XGS_SERVERLISTE", "https://raw.githubusercontent.com/Destinity13/TimeSplitters-2-Native-Linux-Recomp/main/server/servers.txt"); }

		std::vector<std::string> teilen(const std::string& s, char t)
		{
			std::vector<std::string> v;
			std::string cur;
			for (char ch : s)
			{
				if (ch == t) { if (!cur.empty()) v.push_back(cur); cur.clear(); }
				else if (ch != ' ' && ch != '\t' && ch != '\r') cur.push_back(ch);
			}
			if (!cur.empty()) v.push_back(cur);
			return v;
		}

		// "host[:port]" -> Abfrage per UDP; Ergebnis in e (Ping in ms)
		void abfragen(std::vector<Eintrag>& v)
		{
#ifdef _WIN32
			static bool wsa = [] { WSADATA d; return WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
			(void)wsa;
#endif
			struct Offen { size_t i; int fd; std::chrono::steady_clock::time_point t0; uint32_t nonce; };
			std::vector<Offen> offen;
			for (size_t i = 0; i < v.size(); ++i)
			{
				std::string h = v[i].adresse, port = "27713";
				if (!h.empty() && h[0] == '[' && h.find(']') != std::string::npos)
				{
					const size_t e = h.find(']');
					if (e + 1 < h.size() && h[e + 1] == ':') port = h.substr(e + 2);
					h = h.substr(1, e - 1);
				}
				else if (std::count(h.begin(), h.end(), ':') == 1)
				{
					port = h.substr(h.find(':') + 1);
					h = h.substr(0, h.find(':'));
				}
				addrinfo hint{}, *res = nullptr;
				hint.ai_family = AF_UNSPEC;
				hint.ai_socktype = SOCK_DGRAM;
				if (getaddrinfo(h.c_str(), port.c_str(), &hint, &res) != 0 || !res) continue;
				const int fd = static_cast<int>(socket(res->ai_family, SOCK_DGRAM, 0));
				if (fd < 0) { freeaddrinfo(res); continue; }
				const uint32_t nonce = static_cast<uint32_t>(i * 2654435761u) ^ static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
				char q[8] = {'T', '2', 'I', 'N'};
				std::memcpy(q + 4, &nonce, 4);
				const auto t0 = std::chrono::steady_clock::now();
				sendto(fd, q, 8, 0, res->ai_addr, static_cast<int>(res->ai_addrlen));
				freeaddrinfo(res);
				offen.push_back({i, fd, t0, nonce});
			}
			const auto ende = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
			while (!offen.empty() && std::chrono::steady_clock::now() < ende)
			{
				std::vector<pollfd> pf;
				for (auto& o : offen) pf.push_back({static_cast<decltype(pollfd::fd)>(o.fd), POLLIN, 0});
				const int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(ende - std::chrono::steady_clock::now()).count());
#ifdef _WIN32
				if (WSAPoll(pf.data(), static_cast<ULONG>(pf.size()), std::max(ms, 1)) <= 0) break;
#else
				if (poll(pf.data(), pf.size(), std::max(ms, 1)) <= 0) break;
#endif
				for (size_t k = pf.size(); k-- > 0;)
				{
					if (!(pf[k].revents & POLLIN)) continue;
					Offen o = offen[k];
					char b[512];
					const int n = static_cast<int>(recv(o.fd, b, sizeof b, 0));
					if (n >= 15 && std::memcmp(b, "T2IN", 4) == 0 && std::memcmp(b + 4, &o.nonce, 4) == 0)
					{
						Eintrag& e = v[o.i];
						auto u16 = [&](int p) { return static_cast<int>(static_cast<unsigned char>(b[p]) | (static_cast<unsigned char>(b[p + 1]) << 8)); };
						e.proto = u16(8);
						e.spieler = u16(10);
						e.max = u16(12);
						e.pw = b[14] != 0;
						const std::string rest(b + 15, static_cast<size_t>(n - 15));
						e.name = rest.substr(0, rest.find('\0'));
						e.motd = rest.find('\0') != std::string::npos ? rest.substr(rest.find('\0') + 1) : "";
						e.ping = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - o.t0).count());
						e.antwort = true;
					}
#ifdef _WIN32
					closesocket(o.fd);
#else
					close(o.fd);
#endif
					offen.erase(offen.begin() + static_cast<long>(k));
				}
			}
			for (auto& o : offen)
#ifdef _WIN32
				closesocket(o.fd);
#else
				close(o.fd);
#endif
		}

		void aktualisieren(const std::string& favoriten, const std::string& aktuell)
		{
			if (laeuft.exchange(true)) return;
			{ std::lock_guard<std::mutex> lk(om); meldung = "Suche Server ..."; }
			std::thread([favoriten, aktuell] {
				std::vector<std::string> adressen;
				std::string fehler;
				// Community-Liste laden (Zeilen "host[:port]   # Kommentar")
				if (auto dl = HTTPDownloader::Create())
				{
					dl->CreateRequest(listenUrl(), [&](s32 code, const std::string&, HTTPDownloader::Request::Data data) {
						if (code != 200) { fehler = "Serverliste nicht erreichbar (" + std::to_string(code) + ")"; return; }
						std::string text(data.begin(), data.end());
						size_t p = 0;
						while (p < text.size())
						{
							size_t e = text.find('\n', p);
							if (e == std::string::npos) e = text.size();
							std::string z = text.substr(p, e - p);
							p = e + 1;
							if (const size_t h = z.find('#'); h != std::string::npos) z.resize(h);
							for (auto& a : teilen(z, ' ')) { adressen.push_back(a); break; }
						}
					});
					dl->WaitForAllRequests();
				}
				std::vector<Eintrag> v;
				auto dazu = [&](const std::string& a, bool fav) {
					for (auto& e : v) if (e.adresse == a) { e.fav = e.fav || fav; return; }
					Eintrag e; e.adresse = a; e.fav = fav; e.name = a; v.push_back(e);
				};
				for (auto& a : teilen(favoriten, ',')) dazu(a, true);
				for (auto& a : adressen) dazu(a, false);
				if (!aktuell.empty()) dazu(aktuell, false);
				abfragen(v);
				int erreichbar = 0;
				for (auto& e : v) erreichbar += e.antwort ? 1 : 0;
				std::lock_guard<std::mutex> lk(om);
				liste = std::move(v);
				auswahl = -1;
				meldung = (fehler.empty() ? "" : fehler + " - ") + std::to_string(erreichbar) + " von " + std::to_string(liste.size()) + " Servern erreichbar";
				laeuft.store(false);
			}).detach();
		}
	}

	// Reiter "Online" im Spielmenue: Spielername, Serverliste (sortier-/filterbar), Verbinden, Favoriten, direkte Adresse
	bool onlineTab(Cfg& c, bool& schliessen)
	{
		using namespace online;
		bool geaendert = false;
		if (nameEingabe[0] == 0 && !c.onlineName.empty()) std::snprintf(nameEingabe, sizeof nameEingabe, "%s", c.onlineName.c_str());
		ImGui::TextColored(ImVec4(0.50f, 1.00f, 0.42f, 1.0f), "%s", tr("Play online"));
		ImGui::TextDisabled("%s", tr("Pick a server and connect. Then confirm \"TS2 Online\" in the game: Start Game hosts, Join Game lists the games on the server."));
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
		if (ImGui::InputText(tr("Player name"), nameEingabe, sizeof nameEingabe)) { c.onlineName = nameEingabe; geaendert = true; }
		ImGui::SameLine();
		ImGui::TextDisabled("  Server: %s", c.onlineServer.empty() ? tr("(none)") : c.onlineServer.c_str());
		{
			std::lock_guard<std::mutex> lk(om);
			if (!spielStatus.empty()) ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", spielStatus.c_str());
		}
		if (ImGui::Button(laeuft.load() ? tr("Searching ...") : tr("Refresh")) && !laeuft.load()) aktualisieren(c.onlineFavs, c.onlineServer);
		ImGui::SameLine();
		ImGui::Checkbox(tr("not full"), &nurFrei);
		ImGui::SameLine();
		ImGui::Checkbox(tr("no password"), &nurOffen);
		ImGui::SameLine();
		ImGui::Checkbox(tr("favourites only"), &nurFav);
		ImGui::SameLine();
		{ std::lock_guard<std::mutex> lk(om); ImGui::TextDisabled("%s", meldung.c_str()); }

		std::string verbinden;
		bool verbindenPw = false;
		const float tabH = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 3.2f;
		if (ImGui::BeginTable("##server", 6, ImGuiTableFlags_Sortable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable,
				ImVec2(0, std::max(tabH, ImGui::GetFontSize() * 6))))
		{
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("*", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, ImGui::GetFontSize() * 1.4f);
			ImGui::TableSetupColumn(tr("Server"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(tr("Players"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_PreferSortDescending, ImGui::GetFontSize() * 4.5f);
			ImGui::TableSetupColumn(tr("Ping"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort, ImGui::GetFontSize() * 4.0f);
			ImGui::TableSetupColumn(tr("PW"), ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 2.2f);
			ImGui::TableSetupColumn(tr("Message"), ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			std::lock_guard<std::mutex> lk(om);
			if (ImGuiTableSortSpecs* sp = ImGui::TableGetSortSpecs(); sp && sp->SpecsDirty && sp->SpecsCount > 0)
			{
				const int col = sp->Specs[0].ColumnIndex;
				const bool ab = sp->Specs[0].SortDirection == ImGuiSortDirection_Descending;
				std::stable_sort(liste.begin(), liste.end(), [&](const Eintrag& a, const Eintrag& b) {
					if (a.antwort != b.antwort) return a.antwort; // erreichbare zuerst
					int x = 0;
					if (col == 1) x = a.name.compare(b.name);
					else if (col == 2) x = a.spieler - b.spieler;
					else if (col == 3) x = a.ping - b.ping;
					else if (col == 4) x = int(a.pw) - int(b.pw);
					else if (col == 5) x = a.motd.compare(b.motd);
					return ab ? x > 0 : x < 0;
				});
				sp->SpecsDirty = false;
				auswahl = -1;
			}
			for (int i = 0; i < static_cast<int>(liste.size()); ++i)
			{
				Eintrag& e = liste[static_cast<size_t>(i)];
				if (nurFrei && e.antwort && e.spieler >= e.max) continue;
				if (nurOffen && e.pw) continue;
				if (nurFav && !e.fav) continue;
				ImGui::TableNextRow();
				ImGui::PushID(i);
				ImGui::TableNextColumn();
				if (ImGui::SmallButton(e.fav ? "*##f" : ".##f"))
				{
					e.fav = !e.fav;
					std::string f;
					for (auto& x : liste) if (x.fav) f += (f.empty() ? "" : ",") + x.adresse;
					c.onlineFavs = f;
					geaendert = true;
				}
				ImGui::TableNextColumn();
				const bool aktuell = e.adresse == c.onlineServer;
				if (ImGui::Selectable((e.antwort ? e.name : e.adresse + tr(" (no answer)")).c_str(), auswahl == i || aktuell,
						ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap))
				{
					auswahl = i;
					if (ImGui::IsMouseDoubleClicked(0) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown) || ImGui::IsKeyPressed(ImGuiKey_Enter))
					{ verbinden = e.adresse; verbindenPw = e.pw; }
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", e.adresse.c_str(), e.proto && e.proto != 1 ? tr("  (other version)") : "");
				ImGui::TableNextColumn();
				if (e.antwort) ImGui::Text("%d/%d", e.spieler, e.max); else ImGui::TextDisabled("-");
				ImGui::TableNextColumn();
				if (e.antwort) ImGui::TextColored(e.ping < 60 ? ImVec4(0.5f, 0.9f, 0.5f, 1) : e.ping < 120 ? ImVec4(0.95f, 0.85f, 0.4f, 1) : ImVec4(0.95f, 0.5f, 0.4f, 1), "%d ms", e.ping);
				else ImGui::TextDisabled("-");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(e.pw ? tr("yes") : "");
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(e.motd.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}
		{
			std::lock_guard<std::mutex> lk(om);
			const bool sel = auswahl >= 0 && auswahl < static_cast<int>(liste.size());
			if (!sel) ImGui::BeginDisabled();
			if (ImGui::Button(tr("  Connect  ")) && sel) { verbinden = liste[static_cast<size_t>(auswahl)].adresse; verbindenPw = liste[static_cast<size_t>(auswahl)].pw; }
			if (!sel) ImGui::EndDisabled();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
		ImGui::InputTextWithHint("##adr", tr("address (host:port)"), eingabe, sizeof eingabe);
		ImGui::SameLine();
		if (ImGui::Button(tr("Connect to address")) && eingabe[0]) verbinden = eingabe;
		ImGui::SameLine();
		if (ImGui::Button(tr("Add favourite")) && eingabe[0])
		{
			c.onlineFavs += (c.onlineFavs.empty() ? "" : ",") + std::string(eingabe);
			geaendert = true;
			aktualisieren(c.onlineFavs, c.onlineServer);
		}
		ImGui::TextDisabled("%s", tr("Host your own: ts2-server (see server/ in the project). Permanent listing: server/servers.txt."));

		if (!verbinden.empty())
		{
			if (verbindenPw) { pwFuer = verbinden; pwEingabe[0] = 0; ImGui::OpenPopup("##pwpopup"); }
			else
			{
				c.onlineServer = verbinden;
				c.onlinePw.clear();
				geaendert = true;
				std::lock_guard<std::mutex> lk(om);
				spielStatus = tr("Selected: ") + verbinden + tr(" - now confirm \"TS2 Online\" in the game");
				if (vonSpielGeoeffnet) schliessen = true;
			}
		}
		if (ImGui::BeginPopupModal("##pwpopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
		{
			ImGui::Text(tr("Password for %s"), pwFuer.c_str());
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			const bool enter = ImGui::InputText("##pw", pwEingabe, sizeof pwEingabe, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
			if (ImGui::Button("OK") || enter)
			{
				c.onlineServer = pwFuer;
				c.onlinePw = pwEingabe;
				geaendert = true;
				{ std::lock_guard<std::mutex> lk(om); spielStatus = tr("Selected: ") + pwFuer + tr(" - now confirm \"TS2 Online\" in the game"); }
				if (vonSpielGeoeffnet) schliessen = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
		return geaendert;
	}

	void draw()
	{
		static bool styled = false;
		if (online::oeffnen.exchange(false) && online::aktiv() && active.load())
		{
			online::vonSpielGeoeffnet = true;
			s_onlineTabWaehlen = true;
			open.store(true);
			Cfg c0; { std::lock_guard<std::mutex> lk(m); c0 = cfg; }
			online::aktualisieren(c0.onlineFavs, c0.onlineServer);
		}
		if (!open.load()) online::vonSpielGeoeffnet = false;
		if (const double at = applyAt.load(); at > 0.0 && (ImGui::GetTime() >= at || !open.load()))
		{
			applyAt.store(0.0);
			applyPending.store(true);
		}
		if (!open.load())
		{
			s_prevNav = 0;
			// Die ersten Sekunden: dezenter Hinweis, wie das Menue aufgeht
			static const auto s_t0 = std::chrono::steady_clock::now();
			const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_t0).count();
			{
				// Eingabe-Anzeige (Debug-Option, Standard aus): was das Spiel gerade vom Pad/der Tastatur bekommt
				bool show; { std::lock_guard<std::mutex> lk(m); show = cfg.showInput; }
				if (show)
				{
					const u32 b = ps2xosd::btns.load(), ax = ps2xosd::axes.load(), kb = ps2xosd::keyBtns.load();
					static const char* kN[16] = {"SEL", "L3", "R3", "START", "UP", "RIGHT", "DOWN", "LEFT", "L2", "R2", "L1", "R1", "TRI", "O", "X", "SQ"};
					std::string txt = fmt::format("L {:3d},{:3d}  R {:3d},{:3d} ", (ax & 0xFF), ((ax >> 8) & 0xFF), ((ax >> 16) & 0xFF), (ax >> 24));
					for (int i = 0; i < 16; ++i) if (b & (1u << i)) { txt += kN[i]; txt += (kb & (1u << i)) ? "(Tast) " : " "; }
					ImDrawList* dl = ImGui::GetForegroundDrawList();
					const float fs = ImGui::GetFontSize();
					const ImVec2 p0(fs * 1.2f, fs * 1.0f);
					const ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
					dl->AddRectFilled(ImVec2(p0.x - fs * 0.4f, p0.y - fs * 0.3f), ImVec2(p0.x + ts.x + fs * 0.4f, p0.y + ts.y + fs * 0.3f), IM_COL32(0, 0, 0, 150), fs * 0.2f);
					dl->AddText(p0, IM_COL32(120, 255, 140, 255), txt.c_str());
				}
			}
			{
				std::string tt; std::chrono::steady_clock::time_point ta;
				{ std::lock_guard<std::mutex> lk(ps2xhot::tm); tt = ps2xhot::toastText; ta = ps2xhot::toastAt; }
				const float age = std::chrono::duration<float>(std::chrono::steady_clock::now() - ta).count();
				if (!tt.empty() && age < 2.5f)
				{
					const float al = std::clamp((2.5f - age) / 0.5f, 0.0f, 1.0f);
					ImDrawList* dl = ImGui::GetForegroundDrawList();
					const float fs = ImGui::GetFontSize();
					const ImVec2 ts = ImGui::CalcTextSize(tt.c_str());
					const ImVec2 p0(ImGui::GetIO().DisplaySize.x - ts.x - fs * 1.6f, fs * 1.2f);
					dl->AddRectFilled(ImVec2(p0.x - fs * 0.6f, p0.y - fs * 0.4f), ImVec2(p0.x + ts.x + fs * 0.6f, p0.y + ts.y + fs * 0.4f), IM_COL32(10, 10, 8, static_cast<int>(170 * al)), fs * 0.3f);
					dl->AddText(p0, IM_COL32(245, 222, 160, static_cast<int>(255 * al)), tt.c_str());
				}
			}
			if (t < 15.0f)
			{
				const float a = std::clamp((15.0f - t) / 2.0f, 0.0f, 1.0f);
				const char* txt = tr("Menu: ESC / F1   |   Gamepad: Select + Start (or Guide)");
				ImDrawList* dl = ImGui::GetForegroundDrawList();
				const float fs = ImGui::GetFontSize();
				const ImVec2 ts = ImGui::CalcTextSize(txt);
				const ImVec2 p0(fs * 1.2f, ImGui::GetIO().DisplaySize.y - ts.y - fs * 1.6f);
				dl->AddRectFilled(ImVec2(p0.x - fs * 0.6f, p0.y - fs * 0.4f), ImVec2(p0.x + ts.x + fs * 0.6f, p0.y + ts.y + fs * 0.4f), IM_COL32(10, 10, 8, static_cast<int>(170 * a)), fs * 0.3f);
				dl->AddText(p0, IM_COL32(245, 222, 160, static_cast<int>(255 * a)), txt);
			}
			return;
		}
		feedInput();
		if (!styled) { style(); styled = true; }
		ImGuiIO& io = ImGui::GetIO();
		const ImVec2 ds = io.DisplaySize;
		// Hintergrund abdunkeln
		ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(8, 8, 6, 150));
		const float w = std::min(ds.x * 0.62f, 980.0f * ImGuiManager::GetGlobalScale());
		const float h = std::min(ds.y * 0.80f, 820.0f * ImGuiManager::GetGlobalScale());
		ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
		ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
		ImGui::Begin("##ps2xmenu", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

		Cfg c;
		{ std::lock_guard<std::mutex> lk(m); c = cfg; }
		const Cfg before = c;
		bool closeReq = false, quitReq = false;
		sprache.store(c.lang < 0 ? spracheAuto() : c.lang);

		// Kopf: Spieltitel (cyan), rechts Sprache; darunter gruene Zeile mit den Tasten
		{
			const ImVec4 cyan(0.42f, 0.86f, 1.00f, 1.0f), green(0.50f, 1.00f, 0.42f, 1.0f);
			ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.7f);
			ImGui::TextColored(cyan, "%s", spielTitel());
			ImGui::PopFont();
			ImGui::SameLine(0, ImGui::GetFontSize() * 1.2f);
			ImGui::TextColored(green, "%s", tr("Settings"));
			const char* kLang[] = {"Auto", "English", "Deutsch"};
			const float lw = ImGui::GetFontSize() * 7.5f;
			ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - lw);
			ImGui::SetNextItemWidth(lw);
			int li = c.lang + 1;
			if (ImGui::Combo("##lang", &li, kLang, 3)) c.lang = li - 1;
			ImGui::TextDisabled("%s", tr("ESC / F1 / Guide: menu   F7: free camera   F8: record video   F9: FPS   F10: turbo   F11 or Alt+Enter: fullscreen   F12: screenshot"));
			ImGui::Spacing();
		}

		const auto head = [](const char* t) { ImGui::SeparatorText(t); };
		// Inhalt scrollt in einem eigenen Bereich, die Fusszeile bleibt immer sichtbar
		const float fussH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
		ImGui::BeginChild("##inhalt", ImVec2(0, -fussH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
		if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll))
		{
			if (ImGui::BeginTabItem(tr("Display")))
			{
				ImGui::TextDisabled("%s:", tr("Preset"));
				ImGui::SameLine();
				if (ImGui::Button(tr("Remaster (sharp)")))
				{ c.upscale = 4; c.aniso = 16; c.filter = 2; c.cas = 1; c.casSharp = 25; c.fxaa = false; c.tv = 0; c.shadeboost = false; c.dither = 0; c.camBlur = 1; c.nachzieh = 0; c.aspect = 0; }
				ImGui::SameLine();
				if (ImGui::Button(tr("Original look")))
				{ c.upscale = 1; c.aniso = 0; c.filter = 2; c.cas = 0; c.fxaa = false; c.tv = 5; c.shadeboost = false; c.aspect = 2; c.dither = 2; c.camBlur = 1; c.nachzieh = 1; }
				ImGui::SameLine();
				if (ImGui::Button(tr("Performance")))
				{ c.upscale = 2; c.aniso = 4; c.filter = 2; c.cas = 1; c.casSharp = 40; c.fxaa = false; c.tv = 0; }
				head(tr("Resolution & filtering"));
				static const char* kRes[] = {"1x  original (512x448)", "2x  (~720p)", "3x  (~1080p)", "4x  (~1440p)", "5x", "6x  (~4K)", "7x", "8x  (~5K)"};
				int r = c.upscale - 1;
				if (combo(tr("Internal resolution"), &r, kRes, 8)) c.upscale = r + 1;
				static const char* kAspEn[] = {"16:9 widescreen (more view)", "As set in the game's options", "4:3 original", "Fill window (stretched)"};
				static const char* kAspDe[] = {"16:9 Breitbild (mehr Sichtfeld)", "Wie im Spiel-Optionsmenue", "4:3 Original", "Fenster fuellen (gestreckt)"};
				combo(tr("Aspect ratio"), &c.aspect, sprache.load() == 1 ? kAspDe : kAspEn, 4);
				if (c.aspect != before.aspect)
					ImGui::TextDisabled("%s", tr("Takes full effect after the next area change or restart."));
				static const char* kFiltEn[] = {"Pixelated (nearest)", "Bilinear (always)", "Bilinear (as PS2)", "Bilinear (no sprites)"};
				static const char* kFiltDe[] = {"Pixelig (Nearest)", "Bilinear (immer)", "Bilinear (wie PS2)", "Bilinear (ohne Sprites)"};
				combo(tr("Texture filtering"), &c.filter, sprache.load() == 1 ? kFiltDe : kFiltEn, 4);
				static const char* kAniEn[] = {"Off", "2x", "4x", "8x", "16x"};
				static const char* kAniDe[] = {"Aus", "2x", "4x", "8x", "16x"};
				int ai = c.aniso == 0 ? 0 : c.aniso == 2 ? 1 : c.aniso == 4 ? 2 : c.aniso == 8 ? 3 : 4;
				if (combo(tr("Anisotropic filtering"), &ai, sprache.load() == 1 ? kAniDe : kAniEn, 5)) c.aniso = ai == 0 ? 0 : (1 << ai);
				static const char* kTriEn[] = {"Automatic", "As PS2", "Forced (recommended with texture packs)"};
				static const char* kTriDe[] = {"Automatisch", "Wie PS2", "Erzwungen (empfohlen mit Texturpaketen)"};
				combo(tr("Trilinear filtering"), &c.tri, sprache.load() == 1 ? kTriDe : kTriEn, 3);
				head(tr("Window"));
				ImGui::Checkbox(tr("Fullscreen"), &c.fullscreen);
				ImGui::SameLine(0, ImGui::GetFontSize() * 2);
				ImGui::Checkbox(tr("V-Sync"), &c.vsync);
				ImGui::SameLine(0, ImGui::GetFontSize() * 2);
				ImGui::Checkbox(tr("Show FPS"), &c.showFps);
				ImGui::Checkbox(tr("Open this menu at start"), &c.startMenu);
				ImGui::SameLine(0, ImGui::GetFontSize() * 2);
				ImGui::Checkbox(tr("Input overlay (debug)"), &c.showInput);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Picture")))
			{
				{
					bool nz = c.nachzieh != 0;
					if (ImGui::Checkbox(tr("Original frame blending (ghosting when turning)"), &nz)) c.nachzieh = nz ? 1 : 0;
					ImGui::TextDisabled("%s", tr("The PS2 game blends the previous frame half over the new one. Off = clear picture."));
				}
				ImGui::Checkbox(tr("Anti-aliasing (FXAA)"), &c.fxaa);
				static const char* kCasEn[] = {"Off", "Sharpen", "Sharpen + upscale"};
				static const char* kCasDe[] = {"Aus", "Schaerfen", "Schaerfen + Skalieren"};
				combo(tr("Sharpening (AMD CAS)"), &c.cas, sprache.load() == 1 ? kCasDe : kCasEn, 3);
				if (c.cas) ImGui::SliderInt(tr("Sharpening strength"), &c.casSharp, 0, 100);
				static const char* kDithEn[] = {"Off (clean, recommended)", "Scaled", "Original (unscaled)"};
				static const char* kDithDe[] = {"Aus (sauber, empfohlen)", "Skaliert", "Original (unskaliert)"};
				combo(tr("PS2 dithering"), &c.dither, sprache.load() == 1 ? kDithDe : kDithEn, 3);
				static const char* kTvEn[] = {"None", "Scanlines", "Diagonal", "Triangular", "Wave", "CRT (Lottes)", "4x RGSS", "NxAGSS"};
				static const char* kTvDe[] = {"Kein Filter", "Scanlines", "Diagonal", "Dreieck", "Welle", "CRT (Lottes)", "4x RGSS", "NxAGSS"};
				combo(tr("TV / CRT filter"), &c.tv, sprache.load() == 1 ? kTvDe : kTvEn, 8);
				head(tr("Colour"));
				ImGui::Checkbox(tr("Colour adjustment"), &c.shadeboost);
				if (c.shadeboost)
				{
					ImGui::SliderInt(tr("Brightness"), &c.sbB, 1, 100);
					ImGui::SliderInt(tr("Contrast"), &c.sbC, 1, 100);
					ImGui::SliderInt(tr("Saturation"), &c.sbS, 1, 100);
					if (ImGui::Button(tr("Reset colours"))) { c.sbB = c.sbC = c.sbS = 50; }
				}
#ifndef _WIN32
				head(tr("Post effects (vkBasalt)"));
				static const char* kFxEn[] = {"Off", "Original+ (SMAA only)", "Clear (SMAA + sharpen + contrast)", "Cinematic (SMAA + sharpen + colour grade)"};
				static const char* kFxDe[] = {"Aus", "Original+ (nur SMAA)", "Klar (SMAA + Schaerfe + Kontrast)", "Filmisch (SMAA + Schaerfe + Farblook)"};
				combo("##fx", &c.fx, sprache.load() == 1 ? kFxDe : kFxEn, 4);
				ImGui::TextDisabled("%s", tr("Applies at the next start. Home key toggles the effects in game. Needs vkBasalt installed."));
#endif
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Audio")))
			{
				ImGui::SliderInt(tr("Volume"), &c.volume, 0, 200, "%d %%");
				ImGui::TextDisabled("%s", tr("Original sound chip (SPU2) at 48 kHz. Sound is paused while this menu is open."));
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Controls")))
			{
				head(tr("Gamepad"));
				ImGui::Checkbox(tr("Rumble"), &c.rumble);
				if (c.rumble) { ImGui::SameLine(0, ImGui::GetFontSize() * 2); ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12); ImGui::SliderInt(tr("Rumble strength"), &c.rumblePct, 0, 100, "%d %%"); }
				ImGui::TextDisabled("%s", tr("Any modern gamepad is detected automatically (Xbox, PlayStation, Switch, Deck)."));
				head(tr("Mouse"));
				ImGui::Checkbox(tr("Mouse aiming"), &c.maus);
				if (c.maus)
				{
					float sens = static_cast<float>(c.mausSens) / 10.f;
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
					if (ImGui::SliderFloat(tr("Sensitivity"), &sens, 0.1f, 10.f, "%.1f"))
						c.mausSens = std::clamp(static_cast<int>(sens * 10.f + 0.5f), 1, 100);
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
					ImGui::SliderInt(tr("Zoom sensitivity"), &c.zoomSens, 5, 200, "%d %%");
					ImGui::Checkbox(tr("Invert vertical"), &c.mausInvert);
					ImGui::Checkbox(tr("Original crosshair while using the mouse (holds aim mode)"), &c.mausZielmodus);
					ImGui::TextDisabled("%s", tr("Click into the game to capture the mouse, ESC or F1 releases it."));
				}
				head(tr("Key bindings"));
				ImGui::TextDisabled("%s", tr("Click a field, then press a key or mouse button. Delete clears, ESC cancels."));
				{
					// Ergebnis aus dem Fenster-Thread uebernehmen
					const int la = lernAktion.load();
					if (la >= 0)
					{
						const int code = lernCode.exchange(0);
						if (code == -1) lernAktion.store(-1);
						else if (code != 0)
						{
							const int pl = lernPlatz.load();
							const int neu = code == 0xffff ? 0 : code; // Entf leert
							if (neu) for (int i = 0; i < kAnzAktionen; ++i) for (int j = 0; j < 2; ++j) if (c.bel[i][j] == neu) c.bel[i][j] = 0; // doppelte entfernen
							c.bel[la][pl] = neu;
							lernAktion.store(-1);
						}
					}
				}
				if (ImGui::BeginTable("belegung", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
				{
					ImGui::TableSetupColumn(tr("Action"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
					ImGui::TableSetupColumn(tr("Primary"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
					ImGui::TableSetupColumn(tr("Secondary"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
					ImGui::TableHeadersRow();
					for (int i = 0; i < kAnzAktionen; ++i)
					{
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::TextUnformatted(tr(kAktionen[i].name));
						for (int j = 0; j < 2; ++j)
						{
							ImGui::TableNextColumn();
							const bool lernt = lernAktion.load() == i && lernPlatz.load() == j;
							const std::string t = (lernt ? std::string(tr("... press a key ...")) : tastenName(c.bel[i][j])) + "##b" + std::to_string(i * 2 + j);
							if (ImGui::Button(t.c_str(), ImVec2(-1.f, 0.f)) && !lernt)
							{
								lernCode.store(0); lernPlatz.store(j); lernAb.store(ImGui::GetTime() + 0.25); lernAktion.store(i);
							}
						}
					}
					ImGui::EndTable();
				}
				if (ImGui::Button(tr("Reset to defaults")))
				{
					for (int i = 0; i < kAnzAktionen; ++i) { c.bel[i][0] = kAktionen[i].std1; c.bel[i][1] = kAktionen[i].std2; }
					c.belVersion = kBelegungVersion;
				}
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Textures")))
			{
				static std::vector<std::string> quellen; static double lastScan = -1e9;
				static std::vector<std::string> actCache; static std::string actKey;
				if (ImGui::GetTime() - lastScan > 2.0) { quellen = listSources(); lastScan = ImGui::GetTime(); actKey.clear(); }
				static unsigned linked = 0; static bool dirty = false;
				if (ImGui::Checkbox(tr("HD text (own re-typesetting for text pages without a pack)"), &c.hdText)) dirty = true;
				head(tr("Texture packs (PCSX2 format)"));
				if (actKey != c.packs || actKey.empty()) { actCache = activePacks(c.packs); actKey = c.packs.empty() ? std::string("-") : c.packs; }
				const auto& act = actCache;
				if (quellen.empty()) ImGui::TextDisabled("%s", tr("no packs found"));
				for (const auto& q : quellen)
				{
					bool on = std::find(act.begin(), act.end(), q) != act.end();
					if (ImGui::Checkbox(q.c_str(), &on)) { setPackActive(c.packs, q, on); dirty = true; }
				}
				ImGui::TextDisabled(tr("New packs: put the archive into the project's 'texturpakete' folder, %s imports it."), startName());
				ImGui::TextDisabled(tr("Or extracted to %s/<name>/"), quellenDir().c_str());
				if (ImGui::Button(dirty ? tr("Apply and reload textures") : tr("Reload textures")))
				{
					{ std::lock_guard<std::mutex> lk(m); cfg = c; }
					linked = assembleTextures();
					if (g_gs_renderer) { GSTextureReplacements::ReloadReplacementMap(); g_gs_renderer->PurgeTextureCache(true, false, true); }
					dirty = false;
				}
				if (linked) { ImGui::SameLine(); ImGui::TextDisabled(tr("%u files active"), linked); }
				ImGui::TextDisabled("%s", tr("Large packs are preloaded into video memory (short pause)."));
				ImGui::EndTabItem();
			}
			if (online::aktiv() && ImGui::BeginTabItem(tr("Online"), nullptr, s_onlineTabWaehlen ? ImGuiTabItemFlags_SetSelected : 0))
			{
				s_onlineTabWaehlen = false;
				static bool einmal = false;
				if (!einmal) { einmal = true; if (online::liste.empty()) online::aktualisieren(c.onlineFavs, c.onlineServer); }
				onlineTab(c, closeReq);
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem(tr("Save data")))
			{
				ImGui::TextWrapped("%s", memcardInfo().c_str());
				ImGui::TextDisabled("%s", speicherHinweis());
				ImGui::TextDisabled("%s", tr("A backup copy of the save data is made at every start."));
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::EndChild();
		// Fusszeile: Buttons unten rechts
		{
			const float bw = ImGui::CalcTextSize(tr("  Back to game  ")).x + ImGui::CalcTextSize(tr("  Quit game  ")).x + ImGui::GetStyle().FramePadding.x * 4 + ImGui::GetStyle().ItemSpacing.x;
			ImGui::Separator();
			ImGui::SetCursorPosX(ImGui::GetWindowWidth() - bw - ImGui::GetStyle().WindowPadding.x);
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.45f, 0.22f, 0.95f));
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.65f, 0.30f, 1.0f));
			if (ImGui::Button(tr("  Back to game  ")))
				closeReq = true;
			ImGui::PopStyleColor(2);
			ImGui::SameLine();
			if (ImGui::Button(tr("  Quit game  ")))
				quitReq = true;
		}
		if (ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere(-1);
		ImGui::End();

		const bool changed = !sameCfg(before, c);
		if (changed)
		{
			{ std::lock_guard<std::mutex> lk(m); cfg = c; }
			if (c.fullscreen != before.fullscreen) fsPending.store(true);
			// Anwenden erst, wenn sich ~0,3 s nichts mehr aendert (Schieberegler ziehen = sonst jedes Bild neu anwenden -> Ruckeln)
			applyAt.store(ImGui::GetTime() + 0.3);
			save();
		}
		if (closeReq) { open.store(false); latch.store(true); }
		if (quitReq) { save(); ps2xhot::aufnahmeBeenden(); std::fflush(nullptr); std::_Exit(0); }
	}

	void toggle()
	{
		const bool o = !open.load();
		open.store(o);
		if (!o) latch.store(true);
	}
}

// ---- ps2x: Eingabe direkt im Spielfenster (Tastatur) + Gamepad ueber SDL3 (auch ohne Fokus) ----------------
#include <SDL3/SDL.h>
#ifndef _WIN32
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#endif
namespace ps2xin
{
	// PS2-Tastenbits (1 = gedrueckt), wie libpad: Select L3 R3 Start Up Right Down Left L2 R2 L1 R1 Tri Circle Cross Square
	enum : u16 { SELECT = 0x0001, L3 = 0x0002, R3 = 0x0004, START = 0x0008, UP = 0x0010, RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080,
		L2 = 0x0100, R2 = 0x0200, L1 = 0x0400, R1 = 0x0800, TRI = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000 };
	std::atomic<u32> keyBtns{0};      // Tastatur -> Tasten
	std::atomic<u32> keyStick{0};     // Tastatur -> Sticks: Bits 0-3 links (oben unten links rechts), 4-7 rechts
	std::atomic<u32> padBtns{0};
	std::atomic<u32> padAxes{0x80808080u}; // lx | ly<<8 | rx<<16 | ry<<24
	std::atomic<bool> padOn{false};
	std::atomic<u32> rumble{0}; // lo = kleiner Motor, hi = grosser Motor (0..65535)
	// Mauszielen: relative Bewegung seit der letzten Abfrage durch das Spiel, nur solange die Maus gefangen ist
	std::atomic<int> mausDX{0}, mausDY{0};
	std::atomic<bool> mausGefangen{false};
	std::atomic<bool> mausNeu{false};  // erstes Bewegungsereignis nach dem Fangen: nur Startposition merken
	std::atomic<u32> mausTasten{0};   // (alt, ungenutzt)
	std::atomic<int> mausRad{0};      // (alt, ungenutzt)
	std::atomic<u32> aktivA{0};       // gedrueckte Aktionen (Bit = Aktionsnummer), Tastatur + Maus
	std::atomic<int> impulse[32] = {}; // Mausrad: anstehende kurze Druecke je Aktion
	// Aktion ueber Tastatur/Maus gedrueckt/losgelassen -> keyBtns/keyStick neu berechnen
	void neuBerechnen()
	{
		const u32 a = aktivA.load();
		u32 b = 0, st = 0;
		for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
			if (a & (1u << i))
			{
				const auto& ak = ps2xmenu::kAktionen[i];
				if (ak.wirkung == ps2xmenu::W_TASTE) b |= ak.wert; else if (ak.wirkung == ps2xmenu::W_STICK) st |= ak.wert;
			}
		keyBtns.store(b); keyStick.store(st);
	}
	// Code (KeySym oder kMaus+Knopf) gedrueckt/losgelassen; Rad-Codes erzeugen einen Impuls
	void code(int c, bool down)
	{
		if (c == 0) return;
		int bel[32][2];
		{ std::lock_guard<std::mutex> lk(ps2xmenu::m); std::memcpy(bel, ps2xmenu::cfg.bel, sizeof(bel)); }
		const bool rad = c == ps2xmenu::kMaus + 4 || c == ps2xmenu::kMaus + 5;
		for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
			if (bel[i][0] == c || bel[i][1] == c)
			{
				if (rad) { if (down) impulse[i].fetch_add(1); }
				else if (down) aktivA.fetch_or(1u << i); else aktivA.fetch_and(~(1u << i));
			}
		neuBerechnen();
	}
	bool zoomGehalten()
	{
		const u32 a = aktivA.load();
		for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
			if ((a & (1u << i)) && ps2xmenu::kAktionen[i].wirkung == ps2xmenu::W_ZOOM) return true;
		return false;
	}
#ifndef _WIN32
	Atom wmDelete = None;
#endif
	bool fullscreen = false;
	SDL_Gamepad* gp = nullptr;
	bool sdlOk = false;
	// ps2x: Spieler 2-4 (weitere Gamepads, Splitscreen). Spieler 1 bleibt gp/padBtns/padAxes (Tastatur + erstes Pad).
	SDL_Gamepad* gpx[3] = {nullptr, nullptr, nullptr};
	std::atomic<u32> xBtns[3] = {0, 0, 0};
	std::atomic<u32> xAxes[3] = {0x80808080u, 0x80808080u, 0x80808080u};
	std::atomic<bool> xOn[3] = {false, false, false};
	// Zuletzt benutztes Eingabegeraet: Steam (Deck, Desktop-Modus) schickt zum Gamepad zusaetzlich Tastatur/Maus-
	// Ereignisse (D-Pad = Pfeiltasten, B = Esc ...). Solange das Gamepad aktiv ist, zaehlt die Tastatur nicht als Pad.
	std::atomic<long long> padAktivMs{-100000};
	long long jetztMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
	bool padAktiv() { return jetztMs() - padAktivMs.load() < 1500; }
	// Letzte echte Maus-/Tastatur-Eingabe. Wird danach das Gamepad benutzt, gibt die Bridge die Maus frei
	// (Steam Deck: Touchscreen-Tippen oder Trackpad faengt sonst die Maus, und das Spiel bleibt im Maus-Zielmodus).
	std::atomic<long long> mausAktivMs{-100000};
	bool padSeitMaus() { return padAktivMs.load() > mausAktivMs.load() + 250; }

	void key(KeySym ks, bool down)
	{
		int c = static_cast<int>(ks);
		if (c >= 'A' && c <= 'Z') c += 32; // Gross/klein gleich
		code(c, down);
	}

#ifndef _WIN32
	void toggleFullscreen(Display* d, Window w)
	{
		fullscreen = !fullscreen;
		XEvent e{};
		e.xclient.type = ClientMessage;
		e.xclient.window = w;
		e.xclient.message_type = XInternAtom(d, "_NET_WM_STATE", False);
		e.xclient.format = 32;
		e.xclient.data.l[0] = fullscreen ? 1 : 0;
		e.xclient.data.l[1] = static_cast<long>(XInternAtom(d, "_NET_WM_STATE_FULLSCREEN", False));
		e.xclient.data.l[2] = 0;
		e.xclient.data.l[3] = 1;
		XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
		XFlush(d);
	}

#else
	WINDOWPLACEMENT wpPrev{sizeof(WINDOWPLACEMENT)};
	LONG stylePrev = 0;
	void toggleFullscreen(Display*, Window w)
	{
		fullscreen = !fullscreen;
		if (!w)
			return;
		if (fullscreen)
		{
			stylePrev = GetWindowLongW(w, GWL_STYLE);
			GetWindowPlacement(w, &wpPrev);
			MONITORINFO mi{sizeof(mi)};
			GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
			SetWindowLongW(w, GWL_STYLE, stylePrev & ~WS_OVERLAPPEDWINDOW);
			SetWindowPos(w, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
				mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
		else
		{
			SetWindowLongW(w, GWL_STYLE, stylePrev ? stylePrev : WS_OVERLAPPEDWINDOW);
			SetWindowPlacement(w, &wpPrev);
			SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		}
	}
#endif

	void pollExtraPads();
	void pollPad()
	{
		if (!sdlOk)
		{
			static bool tried = false;
			if (tried)
				return;
			tried = true;
			SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
			sdlOk = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
			if (!sdlOk)
			{
				Console.Error(fmt::format("[ps2x] Gamepad-Unterstuetzung (SDL) nicht verfuegbar: {}", SDL_GetError()));
				return;
			}
			int n0 = 0;
			if (SDL_JoystickID* ids0 = SDL_GetJoysticks(&n0)) SDL_free(ids0);
			Console.WriteLn(fmt::format("[ps2x] SDL bereit, {} Controller gefunden", n0));
		}
		SDL_PumpEvents();
		SDL_UpdateGamepads();
		if (gp && !SDL_GamepadConnected(gp)) { SDL_CloseGamepad(gp); gp = nullptr; }
		if (!gp)
		{
			int n = 0;
			if (SDL_JoystickID* ids = SDL_GetGamepads(&n))
			{
				if (n > 0)
				{
					gp = SDL_OpenGamepad(ids[0]);
					if (gp) Console.WriteLn(fmt::format("[ps2x] Gamepad: {}", SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?"));
				}
				SDL_free(ids);
			}
		}
		if (!gp) { padOn.store(false); padBtns.store(0); padAxes.store(0x80808080u); return; }
		auto btn = [](SDL_GamepadButton b) { return SDL_GetGamepadButton(gp, b); };
		u32 m = 0;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_UP)) m |= UP;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) m |= DOWN;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) m |= LEFT;
		if (btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= RIGHT;
		if (btn(SDL_GAMEPAD_BUTTON_SOUTH)) m |= CROSS;
		if (btn(SDL_GAMEPAD_BUTTON_EAST)) m |= CIRCLE;
		if (btn(SDL_GAMEPAD_BUTTON_WEST)) m |= SQUARE;
		if (btn(SDL_GAMEPAD_BUTTON_NORTH)) m |= TRI;
		if (btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) m |= L1;
		if (btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) m |= R1;
		if (btn(SDL_GAMEPAD_BUTTON_START)) m |= START;
		if (btn(SDL_GAMEPAD_BUTTON_BACK)) m |= SELECT;
		if (btn(SDL_GAMEPAD_BUTTON_LEFT_STICK)) m |= L3;
		if (btn(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) m |= R3;
		if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) m |= L2;
		if (SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) m |= R2;
		auto ax = [](SDL_GamepadAxis a) -> u32 { const int v = SDL_GetGamepadAxis(gp, a); return static_cast<u32>((v + 32768) >> 8) & 0xFFu; };
		{
			// Menue: Guide-Taste oder Select+Start; im Menue steuert das Pad die Navigation
			static bool s_prevToggle = false;
			const bool tog = btn(SDL_GAMEPAD_BUTTON_GUIDE) || (btn(SDL_GAMEPAD_BUTTON_BACK) && btn(SDL_GAMEPAD_BUTTON_START));
			if (tog && !s_prevToggle)
				ps2xmenu::toggle();
			s_prevToggle = tog;
			if (ps2xmenu::open.load())
			{
				const int lx = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), ly = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY);
				u32 nb = 0;
				if ((m & UP) || ly < -20000) nb |= 1;
				if ((m & DOWN) || ly > 20000) nb |= 2;
				if ((m & LEFT) || lx < -20000) nb |= 4;
				if ((m & RIGHT) || lx > 20000) nb |= 8;
				if (m & CROSS) nb |= 16;
				if (m & CIRCLE) nb |= 32;
				static u32 s_prevPadNav = 0;
				const u32 chg = nb ^ s_prevPadNav;
				if (chg) { ps2xmenu::nav.fetch_or(nb & chg); ps2xmenu::nav.fetch_and(~(s_prevPadNav & chg & ~nb)); }
				s_prevPadNav = nb;
				m = 0;
			}
			// Vibration (vom Spiel ueber ps2xgs_rumble)
			static u32 s_lastRumble = 0;
			const u32 r = rumble.load();
			bool on; int pct;
			{ std::lock_guard<std::mutex> lk(ps2xmenu::m); on = ps2xmenu::cfg.rumble; pct = ps2xmenu::cfg.rumblePct; }
			const u32 eff = (on && !ps2xmenu::open.load()) ? r : 0u;
			if (eff != s_lastRumble || eff)
			{
				const u16 lo = static_cast<u16>(std::min(65535u, ((eff & 0xFFFFu) * static_cast<u32>(pct)) / 100u));
				const u16 hi = static_cast<u16>(std::min(65535u, ((eff >> 16) * static_cast<u32>(pct)) / 100u));
				SDL_RumbleGamepad(gp, lo, hi, 100);
				s_lastRumble = eff;
			}
		}
		{
			const auto weg = [](SDL_GamepadAxis a) { const int v = SDL_GetGamepadAxis(gp, a); return v > 9000 || v < -9000; };
			if (m || weg(SDL_GAMEPAD_AXIS_LEFTX) || weg(SDL_GAMEPAD_AXIS_LEFTY) || weg(SDL_GAMEPAD_AXIS_RIGHTX) || weg(SDL_GAMEPAD_AXIS_RIGHTY) ||
			    btn(SDL_GAMEPAD_BUTTON_GUIDE) || SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000 || SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000)
				padAktivMs.store(jetztMs());
		}
		padBtns.store(m);
		padAxes.store(ax(SDL_GAMEPAD_AXIS_LEFTX) | (ax(SDL_GAMEPAD_AXIS_LEFTY) << 8) | (ax(SDL_GAMEPAD_AXIS_RIGHTX) << 16) | (ax(SDL_GAMEPAD_AXIS_RIGHTY) << 24));
		padOn.store(true);
		pollExtraPads();
	}

	// ps2x: Spieler 2-4 = die weiteren angeschlossenen Gamepads in SDL-Reihenfolge (ohne das von Spieler 1)
	void pollExtraPads()
	{
		const SDL_JoystickID first = gp ? SDL_GetGamepadID(gp) : 0;
		for (int i = 0; i < 3; ++i)
			if (gpx[i] && !SDL_GamepadConnected(gpx[i])) { SDL_CloseGamepad(gpx[i]); gpx[i] = nullptr; }
		int n = 0;
		if (SDL_JoystickID* ids = SDL_GetGamepads(&n))
		{
			for (int k = 0; k < n; ++k)
			{
				if (ids[k] == first) continue;
				bool known = false;
				for (int i = 0; i < 3; ++i) if (gpx[i] && SDL_GetGamepadID(gpx[i]) == ids[k]) known = true;
				if (known) continue;
				for (int i = 0; i < 3; ++i)
					if (!gpx[i]) { gpx[i] = SDL_OpenGamepad(ids[k]); if (gpx[i]) Console.WriteLn(fmt::format("[ps2x] Gamepad Spieler {}: {}", i + 2, SDL_GetGamepadName(gpx[i]) ? SDL_GetGamepadName(gpx[i]) : "?")); break; }
			}
			SDL_free(ids);
		}
		for (int i = 0; i < 3; ++i)
		{
			SDL_Gamepad* g = gpx[i];
			if (!g) { xOn[i].store(false); xBtns[i].store(0); xAxes[i].store(0x80808080u); continue; }
			auto b = [g](SDL_GamepadButton x) { return SDL_GetGamepadButton(g, x); };
			u32 m = 0;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_UP)) m |= UP;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) m |= DOWN;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) m |= LEFT;
			if (b(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) m |= RIGHT;
			if (b(SDL_GAMEPAD_BUTTON_SOUTH)) m |= CROSS;
			if (b(SDL_GAMEPAD_BUTTON_EAST)) m |= CIRCLE;
			if (b(SDL_GAMEPAD_BUTTON_WEST)) m |= SQUARE;
			if (b(SDL_GAMEPAD_BUTTON_NORTH)) m |= TRI;
			if (b(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) m |= L1;
			if (b(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) m |= R1;
			if (b(SDL_GAMEPAD_BUTTON_START)) m |= START;
			if (b(SDL_GAMEPAD_BUTTON_BACK)) m |= SELECT;
			if (b(SDL_GAMEPAD_BUTTON_LEFT_STICK)) m |= L3;
			if (b(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) m |= R3;
			if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000) m |= L2;
			if (SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000) m |= R2;
			auto a = [g](SDL_GamepadAxis x) -> u32 { const int v = SDL_GetGamepadAxis(g, x); return static_cast<u32>((v + 32768) >> 8) & 0xFFu; };
			if (ps2xmenu::open.load()) m = 0;
			xBtns[i].store(m);
			xAxes[i].store(a(SDL_GAMEPAD_AXIS_LEFTX) | (a(SDL_GAMEPAD_AXIS_LEFTY) << 8) | (a(SDL_GAMEPAD_AXIS_RIGHTX) << 16) | (a(SDL_GAMEPAD_AXIS_RIGHTY) << 24));
			xOn[i].store(true);
		}
	}
}

namespace ps2xmenu
{
	// CPU-Thread (Live-Wiedergabe): Einstellungen anwenden; waehrend das Menue offen ist (Spiel pausiert) neu zeichnen
	void cpuHook()
	{
		if (applyPending.exchange(false))
		{
			toSettings(s_settings_interface);
			const auto t0 = std::chrono::steady_clock::now();
			VMManager::ApplySettings();
			Console.WriteLn(fmt::format("[ps2xmenu] Einstellungen angewendet ({:.0f} ms)", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()));
		}
		if (fsPending.exchange(false) && s_display && s_window != None)
		{
			bool want;
			{ std::lock_guard<std::mutex> lk(m); want = cfg.fullscreen; }
			if (want != ps2xin::fullscreen)
				ps2xin::toggleFullscreen(s_display, s_window);
		}
		{
			// Ton: Lautstaerke nachfuehren, im Menue anhalten
			static int s_vol = -1;
			static bool s_paused = false;
			int vol;
			{ std::lock_guard<std::mutex> lk(m); vol = cfg.volume; }
			if (vol != s_vol && VMManager::HasValidVM()) { SPU2::SetOutputVolume(static_cast<u32>(vol)); s_vol = vol; }
			const bool o = open.load();
			if (o != s_paused && VMManager::HasValidVM()) { SPU2::SetOutputPaused(o); s_paused = o; }
		}
		if (open.load())
		{
			static auto s_last = std::chrono::steady_clock::now();
			const auto now = std::chrono::steady_clock::now();
			if (now - s_last > std::chrono::milliseconds(16))
			{
				s_last = now;
				MTGS::PresentCurrentFrame();
			}
		}
	}
}

// Tastatur im Spielfenster (X11 und Win32): Hotkeys, Menuesteuerung, sonst PS2-Tasten
static void ps2xKey(KeySym ks, bool down, bool alt)
{
	if (down && ps2xin::mausGefangen.load()) ps2xin::mausAktivMs.store(ps2xin::jetztMs());
	if (ps2xmenu::lernAktion.load() >= 0)
	{
		if (down)
		{
			int c = static_cast<int>(ks);
			if (c >= 'A' && c <= 'Z') c += 32;
			ps2xmenu::lernCode.store(ks == XK_Escape ? -1 : c);
		}
		return;
	}
	// Maus gefangen = Tastatur/Maus ist das aktive Geraet, auch wenn ein Gamepad daneben liegt (Stick-Drift zaehlt als aktiv)
	if (down && ps2xin::padAktiv() && !ps2xin::mausGefangen.load() && !(ks == XK_F7 || ks == XK_F8 || ks == XK_F9 || ks == XK_F10 || ks == XK_F11 || ks == XK_F12 || ks == XK_Print))
	{
		static int s_n = 0;
		if (s_n++ < 3) Console.WriteLn("[ps2x] Tastatur-Ereignis ignoriert (Gamepad aktiv, z. B. Steam-Desktop-Belegung)");
		return;
	}
		if (down && (ks == XK_F11 || (ks == XK_Return && alt)))
		{
			ps2xin::toggleFullscreen(s_display, s_window);
			std::lock_guard<std::mutex> lk(ps2xmenu::m);
			ps2xmenu::cfg.fullscreen = ps2xin::fullscreen;
		}
		else if (down && (ks == XK_Escape || ks == XK_F1))
			ps2xmenu::toggle();
		else if (down && ks == XK_F9)
		{
			bool on;
			{ std::lock_guard<std::mutex> lk(ps2xmenu::m); on = ps2xmenu::cfg.showFps = !ps2xmenu::cfg.showFps; }
			ps2xmenu::applyPending.store(true);
			ps2xmenu::save();
			ps2xhot::toast(on ? "FPS: an / on" : "FPS: aus / off");
		}
		else if (down && ks == XK_F10)
		{
			const bool on = !ps2xhot::turbo.load();
			ps2xhot::turbo.store(on);
			ps2xhot::toast(on ? "Turbo: an / on (F10)" : "Turbo: aus / off");
		}
		else if (down && ks == XK_F7)
		{
			const bool on = !ps2xhot::freecam.load();
			ps2xhot::freecam.store(on);
			ps2xhot::toast(on ? "Free camera: on (WASD / Space / Ctrl, Shift = fast, F7 = off)" : "Free camera: off");
		}
		else if (down && ks == XK_F8)
		{
			if (ps2xhot::aufnahme.load()) ps2xhot::aufnahmeStopp();
			else
			{
				char tb[32]; const std::time_t now = std::time(nullptr);
				std::strftime(tb, sizeof(tb), "%Y%m%d_%H%M%S", std::localtime(&now));
				ps2xhot::aufnahmeStart(ps2xhot::shotDir() + "/ts2_" + tb + ".mp4");
			}
		}
		else if (down && (ks == XK_F12 || ks == XK_Print))
		{
			char tb[32]; const std::time_t now = std::time(nullptr);
			std::strftime(tb, sizeof(tb), "%Y%m%d_%H%M%S", std::localtime(&now));
			const std::string p = ps2xhot::shotDir() + "/ts2_" + tb + ".png";
			MTGS::RunOnGSThread([p]() { GSQueueSnapshot(p); });
			ps2xhot::toast("Screenshot: " + p);
		}
		else if (ps2xmenu::open.load())
		{
			u32 bit = 0;
			switch (ks)
			{
				case XK_Up: case XK_w: bit = 1; break;
				case XK_Down: case XK_s: bit = 2; break;
				case XK_Left: case XK_a: bit = 4; break;
				case XK_Right: case XK_d: bit = 8; break;
				case XK_Return: case XK_KP_Enter: case XK_space: case XK_x: bit = 16; break;
				case XK_BackSpace: case XK_c: bit = 32; break;
				default: break;
			}
			if (bit) { if (down) ps2xmenu::nav.fetch_or(bit); else ps2xmenu::nav.fetch_and(~bit); }
			if (!down) ps2xin::key(ks, false); // im Menue losgelassene Spieltasten nicht haengen lassen
		}
		else
			ps2xin::key(ks, down);
}

// Maustaste (1 links, 2 Mitte, 3 rechts, 4/5 Rad, 8/9 Seite): im Lernmodus erfassen, im Fangmodus als belegte Aktion
static bool ps2xMausLernen(int knopf, bool down)
{
	if (ps2xmenu::lernAktion.load() < 0) return false;
	if (down && ImGui::GetCurrentContext() && ImGui::GetTime() >= ps2xmenu::lernAb.load())
		ps2xmenu::lernCode.store(ps2xmenu::kMaus + knopf);
	return true;
}
static void ps2xMausTaste(int knopf, bool down)
{
	if (down) ps2xin::mausAktivMs.store(ps2xin::jetztMs());
	ps2xin::code(ps2xmenu::kMaus + knopf, down);
}
static bool ps2xMausWill() // darf die Maus gefangen werden/bleiben?
{
	bool an; { std::lock_guard<std::mutex> lk(ps2xmenu::m); an = ps2xmenu::cfg.maus; }
	return an && !ps2xmenu::open.load() && !(ps2xin::mausGefangen.load() && ps2xin::padSeitMaus());
}

#ifdef _WIN32
static std::thread s_win_thread;
static std::mutex s_win_mtx;
static std::condition_variable s_win_cv;
static bool s_win_ready = false;
static constexpr UINT WM_PS2X_DESTROY = WM_APP + 0x101;

static KeySym ps2xVkToKeySym(WPARAM vk, LPARAM lp)
{
	if (vk >= 'A' && vk <= 'Z')
		return static_cast<KeySym>('a' + (vk - 'A'));
	switch (vk)
	{
		case VK_UP: return XK_Up;
		case VK_DOWN: return XK_Down;
		case VK_LEFT: return XK_Left;
		case VK_RIGHT: return XK_Right;
		case VK_RETURN: return (lp & (1 << 24)) ? XK_KP_Enter : XK_Return;
		case VK_SHIFT: return (MapVirtualKeyW(static_cast<UINT>((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? XK_Shift_R : XK_Shift_L;
		case VK_TAB: return XK_Tab;
		case VK_ESCAPE: return XK_Escape;
		case VK_BACK: return XK_BackSpace;
		case VK_SPACE: return XK_space;
		case VK_F1: return XK_F1;
		case VK_F7: return XK_F7;
		case VK_F8: return XK_F8;
		case VK_F9: return XK_F9;
		case VK_F10: return XK_F10;
		case VK_F11: return XK_F11;
		case VK_F12: return XK_F12;
		case VK_SNAPSHOT: return XK_Print;
		case VK_CONTROL: return (lp & (1 << 24)) ? 0xffe4 : 0xffe3;
		case VK_MENU: return 0xffe9;
		case VK_CAPITAL: return 0xffe5;
		case VK_DELETE: return 0xffff;
		case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6: return static_cast<KeySym>(0xffbe + (vk - VK_F1));
		case VK_HOME: return 0xff50;
		case VK_PRIOR: return 0xff55;
		case VK_NEXT: return 0xff56;
		case VK_END: return 0xff57;
		case VK_INSERT: return 0xff63;
		case VK_NUMPAD0: case VK_NUMPAD1: case VK_NUMPAD2: case VK_NUMPAD3: case VK_NUMPAD4:
		case VK_NUMPAD5: case VK_NUMPAD6: case VK_NUMPAD7: case VK_NUMPAD8: case VK_NUMPAD9:
			return static_cast<KeySym>(0xffb0 + (vk - VK_NUMPAD0)); // XK_KP_0..9
		case VK_ADD: return 0xffab;
		case VK_SUBTRACT: return 0xffad;
		case VK_MULTIPLY: return 0xffaa;
		case VK_DIVIDE: return 0xffaf;
		case VK_DECIMAL: return 0xffae;
		default:
			if (vk >= '0' && vk <= '9') return static_cast<KeySym>(vk);
			// Satzzeichen, Umlaute usw.: das Zeichen der aktuellen Tastaturbelegung; im Latin-1-Bereich ist das
			// KeySym gleich dem Zeichencode (wie XLookupKeysym unter X11), darueber Unicode-KeySym
			if (const UINT ch = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0x7FFFFFFFu; ch >= 0x20)
			{
				if (ch >= 'A' && ch <= 'Z') return static_cast<KeySym>('a' + (ch - 'A'));
				if (ch < 0x100) return static_cast<KeySym>(ch);
				return static_cast<KeySym>(0x01000000u + ch);
			}
			return 0;
	}
}

static LRESULT CALLBACK ps2xWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg)
	{
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
		{
			if (lp & (1 << 30)) // Wiederholung beim Halten ignorieren (wie XkbSetDetectableAutoRepeat)
				return 0;
			if (msg == WM_SYSKEYDOWN && wp == VK_F4)
				break;
			if (const KeySym ks = ps2xVkToKeySym(wp, lp))
				ps2xKey(ks, true, msg == WM_SYSKEYDOWN && (lp & (1 << 29)));
			return 0;
		}
		case WM_KEYUP:
		case WM_SYSKEYUP:
			if (const KeySym ks = ps2xVkToKeySym(wp, lp))
				ps2xKey(ks, false, false);
			return 0;
		case WM_SYSCHAR:
			return 0; // kein Signalton bei Alt+Enter
		case WM_SYSCOMMAND:
			if ((wp & 0xFFF0) == SC_KEYMENU)
				return 0;
			break;
		case WM_SIZE:
			s_wi.surface_width = LOWORD(lp);
			s_wi.surface_height = HIWORD(lp);
			[[fallthrough]];
		case WM_MOVE:
			if (ps2xin::mausGefangen.load()) // Fenster verschoben / Vollbild gewechselt: Zeigerbegrenzung nachziehen
			{
				RECT cr; GetClientRect(hwnd, &cr); MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
				ClipCursor(&cr);
			}
			return 0;
		case WM_INPUT:
		{
			if (!ps2xin::mausGefangen.load()) break;
			RAWINPUT ri{}; UINT sz = sizeof(ri);
			if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
				ri.header.dwType == RIM_TYPEMOUSE)
			{
				if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
				{
					ps2xin::mausDX.fetch_add(ri.data.mouse.lLastX);
					ps2xin::mausDY.fetch_add(ri.data.mouse.lLastY);
				}
				else
				{
					// Absolute Bewegung (VM, Remote Desktop, Tablet-Modus): 0..65535 ueber den (virtuellen) Bildschirm,
					// daraus die Differenz zur letzten Position bilden
					static LONG letztX = -1, letztY = -1;
					const bool virt = ri.data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP;
					const LONG bw = GetSystemMetrics(virt ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
					const LONG bh = GetSystemMetrics(virt ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
					const LONG x = static_cast<LONG>(ri.data.mouse.lLastX * static_cast<long long>(bw) / 65535);
					const LONG y = static_cast<LONG>(ri.data.mouse.lLastY * static_cast<long long>(bh) / 65535);
					if (letztX >= 0 && ps2xin::mausGefangen.load())
					{
						ps2xin::mausDX.fetch_add(x - letztX);
						ps2xin::mausDY.fetch_add(y - letztY);
					}
					letztX = x; letztY = y;
				}
			}
			break;
		}
		case WM_MOUSEMOVE:
			ps2xmenu::mouseX.store(GET_X_LPARAM(lp));
			ps2xmenu::mouseY.store(GET_Y_LPARAM(lp));
			return 0;
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
		{
			const u32 bit = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 1u : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? 2u : 4u;
			const bool down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
			const int knopf = bit == 1u ? 1 : bit == 2u ? 3 : 2;
			if (ps2xMausLernen(knopf, down)) return 0;
			if (down) { ps2xmenu::mouseBtn.fetch_or(bit); SetFocus(hwnd); } else ps2xmenu::mouseBtn.fetch_and(~bit);
			if (ps2xin::mausGefangen.load())
				ps2xMausTaste(knopf, down);
			else if (down && bit == 1u && ps2xMausWill())
			{
				// Fangen: Zeiger aufs Fenster beschraenken, Rohdaten (relative Bewegung) einschalten
				RAWINPUTDEVICE rid{0x01, 0x02, 0, hwnd};
				RegisterRawInputDevices(&rid, 1, sizeof(rid));
				RECT cr; GetClientRect(hwnd, &cr); MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&cr), 2);
				ClipCursor(&cr);
				ps2xin::mausDX.store(0); ps2xin::mausDY.store(0);
				ps2xin::mausAktivMs.store(ps2xin::jetztMs()); ps2xin::mausGefangen.store(true);
			}
			return 0;
		}
		case WM_MOUSEWHEEL:
		{
			const int knopf = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 4 : 5;
			if (ps2xMausLernen(knopf, true)) return 0;
			if (ps2xin::mausGefangen.load()) ps2xMausTaste(knopf, true);
			else ps2xmenu::wheel.fetch_add(knopf == 4 ? 1 : -1);
			return 0;
		}
		case WM_XBUTTONDOWN: case WM_XBUTTONUP:
		{
			const int knopf = GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 8 : 9;
			const bool down = msg == WM_XBUTTONDOWN;
			if (ps2xMausLernen(knopf, down)) return TRUE;
			if (ps2xin::mausGefangen.load()) ps2xMausTaste(knopf, down);
			return TRUE;
		}
		case WM_KILLFOCUS:
			if (ps2xin::mausGefangen.exchange(false)) ClipCursor(nullptr);
			ps2xin::aktivA.store(0);
			ps2xin::keyBtns.store(0);
			ps2xin::keyStick.store(0);
			return 0;
		case WM_TIMER:
			ps2xin::pollPad(); // Gamepad ~250 Hz
			if (ps2xin::mausGefangen.load() && !ps2xMausWill()) // Menue geoeffnet / Mauszielen aus -> freigeben
			{
				ps2xin::mausGefangen.store(false);
				ClipCursor(nullptr);
			}
			return 0;
		case WM_SETCURSOR:
			if (LOWORD(lp) == HTCLIENT && !ps2xmenu::open.load()) { SetCursor(nullptr); return TRUE; } // Mauszeiger nur im Menue
			break;
		case WM_CLOSE:
			ps2xhot::aufnahmeBeenden();
			std::fflush(nullptr);
			std::_Exit(0); // Speicherkarte wird beim Speichern sofort geschrieben
		case WM_PS2X_DESTROY:
			DestroyWindow(hwnd);
			return 0;
		case WM_DESTROY:
			PostQuitMessage(0);
			return 0;
		default:
			break;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_window, "Tried to create window when there already was one!");
	if (const char* fs = std::getenv("PS2XGS_FULLSCREEN"); fs && *fs == '1')
		ps2xin::fullscreen = true; // wird nach dem Anzeigen gesetzt
	// Win32: Nachrichten muessen auf dem Thread laufen, der das Fenster erzeugt -> eigener UI-Thread
	s_win_thread = std::thread([] {
		const wchar_t* cls = L"ps2xgs-window";
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = ps2xWndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
		wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
		wc.lpszClassName = cls;
		wc.hIcon = LoadIcon(GetModuleHandleW(nullptr), MAKEINTRESOURCE(1));
		RegisterClassExW(&wc);
		const char* t = ps2xmenu::envOr("PS2X_WINDOW_TITLE", "TimeSplitters 2 (TS2 Native Recomp)");
		wchar_t titel[256] = {};
		MultiByteToWideChar(CP_UTF8, 0, t, -1, titel, 255);
		RECT r{0, 0, WINDOW_WIDTH, WINDOW_HEIGHT};
		AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
		HWND h = CreateWindowExW(0, cls, titel, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
			nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
		if (h)
		{
			ShowWindow(h, SW_SHOW);
			UpdateWindow(h);
			SetForegroundWindow(h);
			RECT cr{};
			GetClientRect(h, &cr);
			s_wi.type = WindowInfo::Type::Win32;
			s_wi.window_handle = h;
			s_wi.surface_width = static_cast<u32>(cr.right - cr.left);
			s_wi.surface_height = static_cast<u32>(cr.bottom - cr.top);
			s_wi.surface_scale = 1.0f;
			SetTimer(h, 1, 4, nullptr);
		}
		{
			std::lock_guard<std::mutex> lk(s_win_mtx);
			s_window = h;
			s_display = h;
			s_win_ready = true;
		}
		s_win_cv.notify_all();
		if (!h)
			return;
		MSG m;
		while (GetMessageW(&m, nullptr, 0, 0) > 0)
		{
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	});
	{
		std::unique_lock<std::mutex> lk(s_win_mtx);
		s_win_cv.wait(lk, [] { return s_win_ready; });
	}
	if (!s_window)
	{
		Console.Error("Failed to create Win32 window");
		if (s_win_thread.joinable()) s_win_thread.join();
		return false;
	}
	if (ps2xin::fullscreen) { ps2xin::fullscreen = false; ps2xin::toggleFullscreen(s_display, s_window); }
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_window)
	{
		PostMessageW(s_window, WM_PS2X_DESTROY, 0, 0);
		if (s_win_thread.joinable()) s_win_thread.join();
		s_window = None;
		s_display = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_window)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	// Die Nachrichten laufen im UI-Thread des Fensters; ohne Fenster (Testlauf) hier nur die Gamepads abfragen
	do
	{
		if (s_shutdown_requested.load())
			return;
		if (forever)
		{
			if (!s_window)
				ps2xin::pollPad();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
	} while (forever && !s_shutdown_requested.load());
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}
#else
bool GSRunner::CreatePlatformWindow()
{
	pxAssertRel(!s_display && s_window == None, "Tried to create window when there already was one!");

	XInitThreads();
	s_display = XOpenDisplay(nullptr);
	if (!s_display)
	{
		Console.Error("Failed to open X11 display");
		return false;
	}

	int screen = DefaultScreen(s_display);
	Window root = RootWindow(s_display, screen);

	s_window = XCreateSimpleWindow(s_display, root, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT, 1,
		BlackPixel(s_display, screen), WhitePixel(s_display, screen));

	if (s_window == None)
	{
		Console.Error("Failed to create X11 window");
		XCloseDisplay(s_display);
		s_display = nullptr;
		return false;
	}

	XStoreName(s_display, s_window, ps2xmenu::envOr("PS2X_WINDOW_TITLE", "Shadow of the Colossus (SotC Recomp)"));
	XSelectInput(s_display, s_window, StructureNotifyMask | KeyPressMask | KeyReleaseMask | FocusChangeMask |
		ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
	XkbSetDetectableAutoRepeat(s_display, True, nullptr); // gehaltene Tasten ohne Los-/Druck-Flackern
	ps2xin::wmDelete = XInternAtom(s_display, "WM_DELETE_WINDOW", False);
	XSetWMProtocols(s_display, s_window, &ps2xin::wmDelete, 1);
	{
		XClassHint* ch = XAllocClassHint();
		if (ch) { ch->res_name = const_cast<char*>(ps2xmenu::cfgName()); ch->res_class = const_cast<char*>(ps2xmenu::cfgName()); XSetClassHint(s_display, s_window, ch); XFree(ch); }
	}
	if (const char* fs = std::getenv("PS2XGS_FULLSCREEN"); fs && *fs == '1')
		ps2xin::fullscreen = true; // wird nach dem Anzeigen gesetzt
	{
		// Ohne WM_HINTS input=True geben manche Fenstermanager (KWin unter XWayland) dem Fenster keinen Tastaturfokus
		if (XWMHints* wh = XAllocWMHints())
		{
			wh->flags = InputHint | StateHint;
			wh->input = True;
			wh->initial_state = NormalState;
			XSetWMHints(s_display, s_window, wh);
			XFree(wh);
		}
	}
	XMapRaised(s_display, s_window);

	s_wi.type = WindowInfo::Type::X11;
	s_wi.display_connection = s_display;
	s_wi.window_handle = reinterpret_cast<void*>(s_window);
	s_wi.surface_width = WINDOW_WIDTH;
	s_wi.surface_height = WINDOW_HEIGHT;
	s_wi.surface_scale = 1.0f;

	XFlush(s_display);
	PumpPlatformMessages();
	if (ps2xin::fullscreen) { ps2xin::fullscreen = false; ps2xin::toggleFullscreen(s_display, s_window); }
	return true;
}

void GSRunner::DestroyPlatformWindow()
{
	if (s_display && s_window != None)
	{
		XDestroyWindow(s_display, s_window);
		s_window = None;
	}

	if (s_display)
	{
		XCloseDisplay(s_display);
		s_display = nullptr;
	}
}

std::optional<WindowInfo> GSRunner::GetPlatformWindowInfo()
{
	WindowInfo wi;
	if (s_display && s_window != None)
		wi = s_wi;
	else
		wi.type = WindowInfo::Type::Surfaceless;
	return wi;
}

void GSRunner::PumpPlatformMessages(bool forever)
{
	if (!s_display)
		return;

	do
	{
		while (XPending(s_display) > 0)
		{
			XEvent event;
			XNextEvent(s_display, &event);

			switch (event.type)
			{
				case ConfigureNotify:
				{
					const XConfigureEvent& configure = event.xconfigure;
					s_wi.surface_width = static_cast<u32>(configure.width);
					s_wi.surface_height = static_cast<u32>(configure.height);
					break;
				}
				case DestroyNotify:
					std::_Exit(0);
				case MapNotify:
					XSetInputFocus(s_display, s_window, RevertToParent, CurrentTime);
					break;
				case FocusIn:
					if (std::getenv("PS2X_KEYLOG"))
						std::fprintf(stderr, "[ps2x] Fokus erhalten\n");
					break;
				case KeyPress:
				case KeyRelease:
				{
					const KeySym ks = XLookupKeysym(&event.xkey, 0);
					const bool down = event.type == KeyPress;
					if (down)
					{
						static int s_keys = 0;
						if (s_keys++ == 0) Console.WriteLn("[ps2x] Tastatur aktiv");
						if (std::getenv("PS2X_KEYLOG")) std::fprintf(stderr, "[ps2x] Taste %lx\n", static_cast<unsigned long>(ks));
					}
					ps2xKey(ks, down, (event.xkey.state & Mod1Mask) != 0);
					break;
				}
				case MotionNotify:
					ps2xmenu::mouseX.store(event.xmotion.x);
					ps2xmenu::mouseY.store(event.xmotion.y);
					if (ps2xin::mausGefangen.load())
					{
						// Relative Bewegung zur letzten Position. Nahe am Rand den Zeiger in die Mitte setzen; das dadurch
						// ausgeloeste Ereignis (genau Mitte) zaehlt nicht. Klappt das Versetzen nicht (manche XWayland-
						// Fassungen), bleibt die Bewegung trotzdem richtig, nur am Rand begrenzt.
						static int s_lx = -1, s_ly = -1, s_warp = 0;
						const int w = static_cast<int>(s_wi.surface_width), h = static_cast<int>(s_wi.surface_height);
						const int cx = w / 2, cy = h / 2, x = event.xmotion.x, y = event.xmotion.y;
						if (s_warp > 0 && x == cx && y == cy) { s_warp = 0; s_lx = cx; s_ly = cy; break; }
						if (s_warp > 0 && --s_warp == 0) { /* Versetzen kam nicht an */ }
						if (s_lx >= 0 && ps2xin::mausNeu.exchange(false) == false)
						{
							ps2xin::mausDX.fetch_add(x - s_lx); ps2xin::mausDY.fetch_add(y - s_ly);
						}
						s_lx = x; s_ly = y;
						if (s_warp == 0 && (x < w / 4 || x > w * 3 / 4 || y < h / 4 || y > h * 3 / 4))
						{
							XWarpPointer(s_display, None, s_window, 0, 0, 0, 0, cx, cy);
							s_warp = 8;
						}
					}
					break;
				case ButtonPress:
				case ButtonRelease:
				{
					const unsigned b = event.xbutton.button;
					const bool down = event.type == ButtonPress;
					if (down) XSetInputFocus(s_display, s_window, RevertToParent, CurrentTime);
					if (ps2xMausLernen(static_cast<int>(b), down)) break;
					if (b == 8 || b == 9) { if (ps2xin::mausGefangen.load()) ps2xMausTaste(static_cast<int>(b), down); break; }
					if (b >= 1 && b <= 3)
					{
						const u32 bit = (b == 1) ? 1u : (b == 3) ? 2u : 4u;
						if (down) ps2xmenu::mouseBtn.fetch_or(bit); else ps2xmenu::mouseBtn.fetch_and(~bit);
						if (ps2xin::mausGefangen.load())
							ps2xMausTaste(static_cast<int>(b), down);
						else if (down && b == 1 && ps2xMausWill() &&
							XGrabPointer(s_display, s_window, True, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
								GrabModeAsync, GrabModeAsync, s_window, None, CurrentTime) == GrabSuccess)
						{
							// Fangen: Zeiger aufs Fenster beschraenkt (unter XWayland: Zeiger-Sperre durch den Compositor)
							ps2xin::mausNeu.store(true);
							ps2xin::mausDX.store(0); ps2xin::mausDY.store(0);
							ps2xin::mausAktivMs.store(ps2xin::jetztMs()); ps2xin::mausGefangen.store(true);
						}
					}
					else if (down && (b == 4 || b == 5))
					{
						if (ps2xin::mausGefangen.load()) ps2xMausTaste(static_cast<int>(b), true);
						else ps2xmenu::wheel.fetch_add(b == 4 ? 1 : -1);
					}
					break;
				}
				case FocusOut:
					if (std::getenv("PS2X_KEYLOG"))
						std::fprintf(stderr, "[ps2x] Fokus verloren\n");
					if (ps2xin::mausGefangen.exchange(false)) XUngrabPointer(s_display, CurrentTime);
					ps2xin::aktivA.store(0);
					ps2xin::keyBtns.store(0);
					ps2xin::keyStick.store(0);
					break;
				case ClientMessage:
					if (static_cast<Atom>(event.xclient.data.l[0]) == ps2xin::wmDelete)
					{
						Console.WriteLn("[ps2x] Fenster geschlossen -> Spiel beenden");
						std::fflush(nullptr);
						std::_Exit(0); // Speicherkarte wird beim Speichern sofort geschrieben
					}
					break;
				default:
					break;
			}
		}

		if (s_shutdown_requested.load())
			return;

		if (forever)
		{
			// Mauszeiger: im Spiel unsichtbar, im Menue (Maussteuerung) sichtbar
			static Cursor s_blank = None;
			static int s_cursorAus = -1;
			const int aus = ps2xmenu::open.load() ? 0 : 1;
			if (s_window != None && aus != s_cursorAus)
			{
				if (s_blank == None)
				{
					char z = 0;
					Pixmap pm = XCreateBitmapFromData(s_display, s_window, &z, 1, 1);
					XColor c{};
					s_blank = XCreatePixmapCursor(s_display, pm, pm, &c, &c, 0, 0);
					XFreePixmap(s_display, pm);
				}
				if (aus) XDefineCursor(s_display, s_window, s_blank); else XUndefineCursor(s_display, s_window);
				XFlush(s_display);
				s_cursorAus = aus;
			}
			if (ps2xin::mausGefangen.load() && !ps2xMausWill()) // Menue geoeffnet / Mauszielen aus -> freigeben
			{
				ps2xin::mausGefangen.store(false);
				for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i) ps2xin::impulse[i].store(0);
				XUngrabPointer(s_display, CurrentTime); XFlush(s_display);
			}
			static u32 s_tick = 0;
			if ((++s_tick & 3u) == 0u)
				ps2xin::pollPad(); // Gamepad ~250 Hz
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	} while (forever && !s_shutdown_requested.load());
}

void GSRunner::StopPlatformMessagePump()
{
	s_shutdown_requested.store(true);
}
#endif // _WIN32
#endif // __APPLE__ / _WIN32 || __linux__


//////////////////////////////////////////////////////////////////////////
// ps2x API (einzige exportierte Symbole der Bibliothek)
//////////////////////////////////////////////////////////////////////////
// Haken im gepatchten PCSX2 (ImGuiOverlays.cpp, GSDumpReplayer.cpp). Auf Dateiebene deklariert: innerhalb der
// extern-"C"-Exportfunktionen haetten sie C-Bindung, was unter Windows (MSVC-Namensschema) nicht zur Definition passt.
extern void (*g_ps2xMenuDraw)();
extern void (*g_ps2xCpuHook)();

#ifdef _WIN32
#define PS2XGS_API extern "C" __declspec(dllexport)
#else
#define PS2XGS_API extern "C" __attribute__((visibility("default")))
#endif

static std::thread s_ps2x_cpu_thread;
static std::thread s_ps2x_pump_thread;
// Beim Entladen der DLL / Prozessende laufen die Threads noch: ein zerstoertes, noch "joinable" std::thread ruft
// std::terminate (Windows: abort -> 0xC0000409 beim Beenden). Dieses Objekt wird vor den Threads zerstoert und loest sie.
static struct Ps2xThreadsLoesen
{
	~Ps2xThreadsLoesen()
	{
		for (std::thread* t : {&s_ps2x_cpu_thread, &s_ps2x_pump_thread})
			if (t->joinable()) t->detach();
#ifdef _WIN32
		if (s_win_thread.joinable()) s_win_thread.detach();
#endif
	}
} s_ps2x_threads_loesen;
static std::atomic<int> s_ps2x_ret{0};
static VMBootParameters s_ps2x_params;

#ifdef _WIN32

#include <crtdbg.h>
// Windows-Diagnose: Bridge-Log in eine Datei (die Windows-Konsole von PCSX2 schreibt nicht in umgeleitete Ausgaben)
// und eigene Absturzmeldung fuer Faelle, die CrashHandler (SEH) nicht sieht: abort()/std::terminate/ungueltige
// CRT-Parameter enden sonst als 0xC0000409 ohne jede Spur.
namespace ps2xdiag
{
	static std::string s_log;
	static void zeile(const char* text)
	{
		if (!s_log.empty())
			if (std::FILE* f = std::fopen(s_log.c_str(), "ab")) { std::fputs(text, f); std::fputc('\n', f); std::fclose(f); }
		std::fputs(text, stderr); std::fputc('\n', stderr); std::fflush(stderr);
		OutputDebugStringA(text);
	}
	static void stapel(const char* grund)
	{
		char b[512];
		std::snprintf(b, sizeof(b), "[ps2xgs] ABSTURZ: %s (Thread %lu)", grund, GetCurrentThreadId());
		zeile(b);
		void* f[48] = {};
		const USHORT n = CaptureStackBackTrace(1, 48, f, nullptr);
		for (USHORT i = 0; i < n; ++i)
		{
			HMODULE m = nullptr; char name[MAX_PATH] = "?";
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(f[i]), &m) && m)
				GetModuleFileNameA(m, name, MAX_PATH);
			const char* kurz = std::strrchr(name, '\\');
			std::snprintf(b, sizeof(b), "  #%02u %s+0x%llx", i, kurz ? kurz + 1 : name,
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(f[i]) - reinterpret_cast<uintptr_t>(m)));
			zeile(b);
		}
	}
	static void beiAbort(int) { stapel("abort() (SIGABRT)"); }
	static void beiParameter(const wchar_t* expr, const wchar_t* fn, const wchar_t* file, unsigned line, uintptr_t)
	{
		char b[600];
		std::snprintf(b, sizeof(b), "ungueltiger CRT-Parameter: %ls in %ls (%ls:%u)", expr ? expr : L"?", fn ? fn : L"?", file ? file : L"?", line);
		stapel(b);
	}
	static void beiTerminate() { stapel("std::terminate (Ausnahme nicht gefangen)"); std::abort(); }
	static LONG WINAPI beiAusnahme(EXCEPTION_POINTERS* e)
	{
		const DWORD c = e && e->ExceptionRecord ? e->ExceptionRecord->ExceptionCode : 0;
		// nur echte Fehler (keine Debug-/C++-Ausnahmen, die gefangen werden)
		if (c == 0xC0000005 || c == 0xC0000409 || c == 0xC000001D || c == 0xC00000FD || c == 0xC0000374 || c == 0xC0000094)
		{
			char b[160];
			HMODULE m = nullptr; char name[MAX_PATH] = "?";
			void* adr = e->ExceptionRecord->ExceptionAddress;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCSTR>(adr), &m) && m)
				GetModuleFileNameA(m, name, MAX_PATH);
			const char* kurz = std::strrchr(name, '\\');
			std::snprintf(b, sizeof(b), "Ausnahme 0x%08lX bei %s+0x%llx", c, kurz ? kurz + 1 : name,
				static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(adr) - reinterpret_cast<uintptr_t>(m)));
			stapel(b);
		}
		return EXCEPTION_CONTINUE_SEARCH;
	}
	static void installieren()
	{
		if (const char* p = std::getenv("PS2XGS_LOG"); p && *p) s_log = p;
		else if (const char* a = std::getenv("APPDATA"); a && *a)
		{
			const char* cn = std::getenv("PS2XGS_CFGNAME");
			s_log = std::string(a) + "\\" + ((cn && *cn) ? cn : "ts2-recomp");
			CreateDirectoryA(s_log.c_str(), nullptr);
			s_log += "\\bridge.log";
		}
		if (!s_log.empty()) if (std::FILE* f = std::fopen(s_log.c_str(), "wb")) std::fclose(f); // je Start neu
		signal(SIGABRT, beiAbort);
		_set_invalid_parameter_handler(beiParameter);
		std::set_terminate(beiTerminate);
		AddVectoredExceptionHandler(1, beiAusnahme);
		zeile("[ps2xgs] Diagnose aktiv");
	}
}
#define PS2XDIAG(text) ps2xdiag::zeile(text)
#else
#define PS2XDIAG(text) ((void)0)
#endif

PS2XGS_API int ps2xgs_start(const char* live_path, int renderer, float upscale, int widescreen)
{
#ifdef _WIN32
	ps2xdiag::installieren();
	// Prozessende: PCSX2 laesst eigene Threads laufen (z. B. Texturersatz-Lader); deren statische std::thread-Objekte
	// wuerden beim Entladen der DLL std::terminate ausloesen (0xC0000409 beim Beenden). Dieser atexit-Eintrag der DLL
	// laeuft vor allen frueher registrierten statischen Destruktoren und beendet den Prozess sofort.
	std::atexit([] { std::fflush(nullptr); TerminateProcess(GetCurrentProcess(), 0); });
#endif
	CrashHandler::Install(); // Absturz-Aufrufstapel ins Log (libbacktrace)
	GSRunner::InitializeConsole();
#ifdef _WIN32
	if (!ps2xdiag::s_log.empty()) Log::SetFileOutputLevel(LOGLEVEL_DEBUG, ps2xdiag::s_log + ".pcsx2");
#endif
	PS2XDIAG("[ps2xgs] start: Konsole");
	if (!GSRunner::InitializeConfig())
	{
		Console.Error("ps2xgs: Konfiguration fehlgeschlagen");
		PS2XDIAG("[ps2xgs] start: Konfiguration fehlgeschlagen");
		return 0;
	}
	PS2XDIAG("[ps2xgs] start: Konfiguration ok");
	// PS2XGS_SURFACELESS=1 (Testlaeufe): ohne sichtbares Fenster rendern, Screenshots gehen trotzdem
	const char *surfaceless = std::getenv("PS2XGS_SURFACELESS");
	ps2xmenu::load(upscale);
	{
		std::lock_guard<std::mutex> lk(ps2xmenu::m);
		if (ps2xmenu::cfg.fullscreen)
			ps2x_setenv("PS2XGS_FULLSCREEN", "1", 0);
	}
	PS2XDIAG("[ps2xgs] start: Menue-Einstellungen geladen");
	if (!(surfaceless && *surfaceless && *surfaceless != '0') && !GSRunner::CreatePlatformWindow())
	{
		Console.Error("ps2xgs: Fenster fehlgeschlagen");
		PS2XDIAG("[ps2xgs] start: Fenster fehlgeschlagen");
		return 0;
	}
	PS2XDIAG("[ps2xgs] start: Fenster ok");
	GSRunner::SettingsOverride();
	// Der Port taktet selbst (VBlank 50 Hz) -> kein eigener Limiter, kein VSync-Warten im GS (V-Sync optional im Menue)
	s_settings_interface.SetBoolValue("EmuCore/GS", "FrameLimitEnable", false);
	s_settings_interface.SetIntValue("EmuCore/GS", "Renderer", renderer);
	s_settings_interface.SetFloatValue("EmuCore/GS", "upscale_multiplier", upscale);
	if (widescreen)
		s_settings_interface.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
	s_settings_interface.SetBoolValue("EmuCore/GS", "OsdShowGSStats", false);
	s_settings_interface.SetBoolValue("Logging", "EnableVerbose", false);
	// SotC-Korrekturen aus PCSX2s GameIndex.yaml (SCES-53326). Ohne Disc-Start greift die Spieldatenbank nicht,
	// daher hier fest: Wasserfarbe (Blending 4), Raender/Versatz (Half-Pixel 5), Nachbearbeitung/DOF (Native Scaling 3)
	// Nur fuer SotC (andere Spiele per PS2XGS_SERIAL bekommen PCSX2-Standard bzw. eigene Werte)
	if (const char* ser = std::getenv("PS2XGS_SERIAL"); !ser || !*ser || std::string_view(ser) == "SCES-53326")
	{
		s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);
		s_settings_interface.SetIntValue("EmuCore/GS", "accurate_blending_unit", 4);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", 5);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_native_scaling", 3);
	}
	else if (std::string_view(ser) == "SLUS-20314") // TimeSplitters 2 (GameIndex.yaml): "Fixes slight texture misalignment"
	{
		s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);
		s_settings_interface.SetIntValue("EmuCore/GS", "UserHacks_HalfPixelOffset", 2);
	}
	// Diagnose: PS2XGS_GSOPT="UserHacks_HalfPixelOffset=0;upscale_multiplier=3" setzt beliebige EmuCore/GS-Werte (Ganzzahl/Kommazahl)
	if (const char* o = std::getenv("PS2XGS_GSOPT"); o && *o)
	{
		std::string_view rest(o);
		while (!rest.empty())
		{
			const size_t sc = rest.find(';');
			const std::string_view kv = rest.substr(0, sc);
			rest = sc == std::string_view::npos ? std::string_view() : rest.substr(sc + 1);
			const size_t eq = kv.find('=');
			if (eq == std::string_view::npos) continue;
			const std::string k(kv.substr(0, eq)), v(kv.substr(eq + 1));
			if (v.find('.') != std::string::npos) s_settings_interface.SetFloatValue("EmuCore/GS", k.c_str(), std::strtof(v.c_str(), nullptr));
			else s_settings_interface.SetIntValue("EmuCore/GS", k.c_str(), std::atoi(v.c_str()));
			if (k.rfind("UserHacks_", 0) == 0) s_settings_interface.SetBoolValue("EmuCore/GS", "UserHacks", true);
			Console.WriteLn(fmt::format("[ps2xgs] GS-Option {} = {}", k, v));
		}
	}
	// Spielmenue: gespeicherte Einstellungen uebernehmen (Testlaeufe ohne Fenster: unveraendert, wie bisher)
	const bool cfgTest = std::getenv("PS2XGS_CFG") != nullptr; // Testlauf mit festen Menue-Einstellungen
	if (cfgTest)
		s_settings_interface.SetIntValue("EmuCore/GS", "ScreenshotSize", 1); // Screenshot in interner Aufloesung
	if (!(surfaceless && *surfaceless && *surfaceless != '0') || cfgTest)
	{
		// Ton: SPU2-Ausgabe ueber Cubeb (PulseAudio/PipeWire), 48 kHz, Zeitdehnung gegen Knacken
		if (!cfgTest)
		{
			s_settings_interface.SetStringValue("SPU2/Output", "Backend", "Cubeb");
			s_settings_interface.SetIntValue("SPU2/Output", "BufferMS", 50);
			s_settings_interface.SetIntValue("SPU2/Output", "OutputLatencyMS", 20);
		}
		ps2xmenu::toSettings(s_settings_interface);
		if (widescreen)
			s_settings_interface.SetStringValue("EmuCore/GS", "AspectRatio", "16:9");
		g_ps2xMenuDraw = &ps2xmenu::draw;
		ps2xmenu::active.store(true);
		g_ps2xCpuHook = &ps2xmenu::cpuHook;
		// Startmenue: Einstellungen vor dem Spielen (abschaltbar im Menue)
		bool sm;
		{ std::lock_guard<std::mutex> lk(ps2xmenu::m); sm = ps2xmenu::cfg.startMenu; }
		if (sm && !cfgTest)
			ps2xmenu::open.store(true);
	}
	s_settings_interface.SetBoolValue("EmuCore", "WarnAboutUnsafeSettings", false); // keine PCSX2-Warnhinweise im Bild
	// Bildausgabe: SotC zeichnet volle Bilder, das PCRTC zeigt sie als Halbbilder (SMODE2 INT=1, FFMD=0). PCSX2s
	// automatisches Deinterlacing (adaptiv) laesst bewegte Figuren auf dem Bildschirm zittern/"wobbeln", in Screenshots
	// nicht sichtbar. Daher: Deinterlacing aus = das volle Bild direkt zeigen. PS2X_DEINTERLACE=<0..9> zum Testen.
	{
		int di = 1; // GSInterlaceMode::Off
		if (const char* e = std::getenv("PS2X_DEINTERLACE"); e && *e) di = std::atoi(e);
		s_settings_interface.SetIntValue("EmuCore/GS", "deinterlace_mode", di);
	}
	{
		// HD-Texturen (Schrift, Logos, Menues): eigener Ordner, PCSX2-Austauschmechanismus
		std::string tex;
		if (const char* t = std::getenv("PS2XGS_TEXDIR"); t && *t) tex = t;
		else tex = ps2xmenu::dir() + "/texturen";
		s_settings_interface.SetStringValue("Folders", "Textures", tex.c_str());
		EmuFolders::Textures = tex; // wurde schon beim Start aus der PCSX2-Konfiguration gesetzt
		{ const unsigned n = ps2xmenu::assembleTextures(); if (n) Console.WriteLn("[ps2x] Texturen: %u Ersatzdateien aktiv (HD-Text/Pakete)", n); }
		s_settings_interface.SetBoolValue("EmuCore/GS", "LoadTextureReplacements", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "LoadTextureReplacementsAsync", true); // nie im Grafik-Thread laden (Ruckler); Aufploppen verhindert das Vorladen beim Start
		s_settings_interface.SetBoolValue("EmuCore/GS", "PrecacheTextureReplacements", true);
		const char* dump = std::getenv("PS2XGS_TEXDUMP");
		const bool d = dump && *dump == '1';
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpReplaceableTextures", d);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpReplaceableMipmaps", false);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpDirectTextures", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpPaletteTextures", true);
		s_settings_interface.SetBoolValue("EmuCore/GS", "DumpTexturesWithFMVActive", false);
		Console.WriteLn(fmt::format("[ps2xgs] HD-Texturen: {}{}", tex, d ? " (Auslesen aktiv)" : ""));
	}

	s_ps2x_params.filename = live_path; // endet auf ps2x_live.gs -> GSDumpReplayer Live-Modus
	s_ps2x_cpu_thread = std::thread(CPUThreadMain, &s_ps2x_params, &s_ps2x_ret);
	PS2XDIAG("[ps2xgs] start: CPU-Thread gestartet");
	s_ps2x_pump_thread = std::thread([] { GSRunner::PumpPlatformMessages(true); });
	// Warten, bis die VM (inkl. SPU2) bereit ist: der Port schreibt danach sofort SPU2-Register (Sound-Treiber-Start)
	// bis zu 30 s: Vulkan legt beim ersten Start auf manchen Treibern erst Pipeline-/Shader-Caches an
	int ms = 0;
	for (; ms < 30000 && !VMManager::HasValidVM(); ms += 10)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	if (VMManager::HasValidVM())
	{
		char b[64]; std::snprintf(b, sizeof(b), "[ps2xgs] start: VM bereit (%d ms)", ms);
		PS2XDIAG(b);
	}
	else
		PS2XDIAG("[ps2xgs] start: VM nach 30 s nicht bereit");
	// PS2XGS_VM_PFLICHT=1 (Windows-Launcher): ohne gueltige VM (z. B. Grafikgeraet liess sich nicht anlegen) Fehlschlag
	// melden, damit der Launcher mit einem anderen Renderer neu starten kann, statt ein schwarzes Fenster zu zeigen
	if (const char* e = std::getenv("PS2XGS_VM_PFLICHT"); e && *e == '1' && !VMManager::HasValidVM())
		return 0;
	return 1;
}

PS2XGS_API void ps2xgs_transfer(int path, const unsigned char* data, unsigned int size)
{
	GSDumpReplayer::LivePush(0, static_cast<u8>(path), data, size);
}

// Gesammelte Grafikpakete des aufrufenden Threads abgeben (Port: nach jedem VU1-Auftrag und vor jeder Uebergabe)
PS2XGS_API void ps2xgs_flush()
{
	GSDumpReplayer::LiveFlush();
}

// GS-Download fuer VIF1-DMA Richtung Speicher (Spiele lesen VRAM zurueck, z. B. TimeSplitters 2):
// offene Pakete abgeben, dann wie PCSX2 den VIF1-FIFO aus dem GS lesen (BITBLTBUF/TRXPOS/TRXREG aus dem GS-Zustand)
PS2XGS_API void ps2xgs_read_fifo(unsigned char* mem, unsigned int qwc)
{
	GSDumpReplayer::LiveReadFifo(mem, qwc);   // auf dem CPU-Thread in Paketreihenfolge, wartet auf Fertig
}

// ps2x: GS-Download ohne den EE anzuhalten (TS2 liest jedes Menuebild zurueck; Ergebnis wird erst beim VBlank gebraucht)
PS2XGS_API void* ps2xgs_read_fifo_async(unsigned char* mem, unsigned int qwc)
{
	return GSDumpReplayer::LiveReadFifoAsync(mem, qwc);
}

PS2XGS_API void ps2xgs_read_fifo_wait(void* ticket)
{
	GSDumpReplayer::LiveReadFifoWait(static_cast<GSDumpReplayer::LiveReadFifoReq*>(ticket));
}

PS2XGS_API void ps2xgs_vsync(const unsigned char* regs0x2000, int field)
{
	// Vorladen der Ersatztexturen (Texturpakete): beim Start einmal warten, bis der Lade-Thread fertig ist, statt
	// die ersten Minuten im Menue nachzuladen (das ruckelte). Hoechstens 45 s; PS2XGS_TEX_WARTEN=0 schaltet es ab.
	static unsigned s_vsyncs = 0;
	// PCSX2-Hinweise, die fuer den Port nicht gelten (keine PCSX2-Patchdatenbank, Eingaben laufen ueber den Port
	// selbst), in den ersten Sekunden wieder entfernen, statt sie als Einblendung oben links zu zeigen
	if (s_vsyncs < 600 && (s_vsyncs % 15u) == 0u)
	{
		Host::RemoveKeyedOSDMessage("PatchesZipOpenWarning");
		Host::RemoveKeyedOSDMessage("ControllerNotConfigured");
	}
	if (++s_vsyncs == 120) // nach den ersten Bildern (interne Aufloesung bekannt)
	{
		if (const char* c = std::getenv("PS2XGS_CAPTURE"); c && *c) ps2xhot::aufnahmeStart(c);
	}
	if (s_vsyncs == 20)
	{
		static const bool warten = [] { const char* e = std::getenv("PS2XGS_TEX_WARTEN"); return !(e && *e == '0'); }();
		const unsigned pending = GSTextureReplacements::PendingWorkerItems();
		if (warten && pending > 0)
		{
			const auto t0 = std::chrono::steady_clock::now();
			const bool ok = GSTextureReplacements::WaitForWorkerIdle(45000);
			Console.WriteLn("[ps2x] Texturen vorgeladen: %u Dateien in %.1f s%s", pending,
				std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), ok ? "" : " (Zeitlimit)");
		}
	}
	GSDumpReplayer::LivePush(3, 0, regs0x2000, 0x2000);
	GSDumpReplayer::LivePush(1, static_cast<u8>(field), nullptr, 0);
}

PS2XGS_API unsigned int ps2xgs_queue_size()
{
	return static_cast<unsigned int>(GSDumpReplayer::LiveQueueSize());
}

PS2XGS_API int ps2xgs_turbo() { return ps2xhot::turbo.load() ? 1 : 0; }
PS2XGS_API void ps2xgs_capture_end() { ps2xhot::aufnahmeBeenden(); }

PS2XGS_API void ps2xgs_screenshot(const char* path)
{
	std::string p(path);
	MTGS::RunOnGSThread([p]() { GSQueueSnapshot(p); });
}

// Eingabe fuer den Port: gedrueckte PS2-Tasten (1 = gedrueckt) und Sticks (lx, ly, rx, ry; 0x80 = Mitte).
// Gamepad und Tastatur (im Spielfenster) werden zusammengefuehrt. Rueckgabe 1, wenn ein Fenster/Gamepad da ist.
PS2XGS_API int ps2xgs_pad(unsigned short* pressed, unsigned char* axes4)
{
	u32 b = ps2xin::keyBtns.load() | ps2xin::padBtns.load();
	{
		// Mausrad-Impulse: je Raste ~6 Abfragen lang die Aktion druecken, dann kurz loslassen
		static int s_halten = 0, s_akt = -1;
		if (s_halten > 0) { --s_halten; if (s_halten == 0) s_halten = -4; }
		else if (s_halten < 0) ++s_halten;
		else
			for (int i = 0; i < ps2xmenu::kAnzAktionen; ++i)
				if (ps2xin::impulse[i].load() > 0) { ps2xin::impulse[i].fetch_sub(1); s_akt = i; s_halten = 6; break; }
		if (s_halten > 0 && s_akt >= 0 && ps2xmenu::kAktionen[s_akt].wirkung == ps2xmenu::W_TASTE) b |= ps2xmenu::kAktionen[s_akt].wert;
		// Zoom (halten): Steuerkreuz hoch, solange gehalten; beim Loslassen genauso lange runter (zurueck auf normal)
		static int s_zoom = 0, s_zurueck = 0;
		if (ps2xin::zoomGehalten()) { b |= ps2xin::UP; if (s_zoom < 240) ++s_zoom; s_zurueck = 0; }
		else if (s_zoom > 0) { s_zurueck = s_zoom; s_zoom = 0; }
		if (s_zurueck > 0) { b |= ps2xin::DOWN; --s_zurueck; }
	}
	u32 ax = ps2xin::padAxes.load();
	if (ps2xmenu::open.load() || ps2xmenu::latch.load())
	{
		// Menue offen bzw. gerade geschlossen: Spiel bekommt nichts, bis alles losgelassen ist
		if (!ps2xmenu::open.load() && b == 0 && (ps2xin::keyStick.load() == 0))
			ps2xmenu::latch.store(false);
		if (pressed) *pressed = 0;
		if (axes4) std::memset(axes4, 0x80, 4);
		return (s_display != nullptr || ps2xin::padOn.load()) ? 1 : 0;
	}
	unsigned char a[4] = {static_cast<unsigned char>(ax), static_cast<unsigned char>(ax >> 8), static_cast<unsigned char>(ax >> 16), static_cast<unsigned char>(ax >> 24)};
	const u32 ks = ps2xin::keyStick.load();
	if (ks & 1) a[1] = 0x00; if (ks & 2) a[1] = 0xFF; if (ks & 4) a[0] = 0x00; if (ks & 8) a[0] = 0xFF;
	if (ks & 16) a[3] = 0x00; if (ks & 32) a[3] = 0xFF; if (ks & 64) a[2] = 0x00; if (ks & 128) a[2] = 0xFF;
	if (pressed) *pressed = static_cast<unsigned short>(b);
	if (axes4) std::memcpy(axes4, a, 4);
	ps2xosd::btns.store(b); ps2xosd::axes.store(static_cast<u32>(a[0]) | (static_cast<u32>(a[1]) << 8) | (static_cast<u32>(a[2]) << 16) | (static_cast<u32>(a[3]) << 24));
	ps2xosd::keyBtns.store(ps2xin::keyBtns.load());
	{
		// Eingabe-Protokoll (letzter_start.log): bei Aenderung, max. ~10/s -> Diagnose Laufen/Stick
		static u32 s_last = 0xFFFFFFFFu, s_lastAx = 0;
		static auto s_t = std::chrono::steady_clock::now();
		const u32 axq = (a[0] >> 4) | ((a[1] >> 4) << 4) | ((a[2] >> 4) << 8) | ((a[3] >> 4) << 12);
		const auto now = std::chrono::steady_clock::now();
		if ((b != s_last || axq != s_lastAx) && now - s_t > std::chrono::milliseconds(100))
		{
			s_last = b; s_lastAx = axq; s_t = now;
			std::fprintf(stderr, "[Eingabe] tasten=%04x links=%02x/%02x rechts=%02x/%02x pad=%d tast=%02x\n", b, a[0], a[1], a[2], a[3],
				ps2xin::padOn.load() ? 1 : 0, ps2xin::keyStick.load());
		}
	}
	return (s_display != nullptr || ps2xin::padOn.load()) ? 1 : 0;
}

// Breitbild: 0 = Spiel entscheidet, 1 = 16:9 erzwingen, 2 = 4:3 erzwingen
// ps2x: Eingabe je Spieler (0 = wie ps2xgs_pad, 1..3 = weitere Gamepads). Rueckgabe 1 = Gamepad vorhanden.
PS2XGS_API int ps2xgs_pad_n(int player, unsigned short* pressed, unsigned char* axes4)
{
	if (player <= 0)
		return ps2xgs_pad(pressed, axes4);
	if (player > 3 || !ps2xin::xOn[player - 1].load())
		return 0;
	const u32 ax = ps2xin::xAxes[player - 1].load();
	if (pressed) *pressed = static_cast<unsigned short>(ps2xin::xBtns[player - 1].load());
	if (axes4) { axes4[0] = ax & 0xFF; axes4[1] = (ax >> 8) & 0xFF; axes4[2] = (ax >> 16) & 0xFF; axes4[3] = (ax >> 24) & 0xFF; }
	return 1;
}

PS2XGS_API int ps2xgs_wide_mode()
{
	if (!ps2xmenu::active.load())
		return 0;
	int a;
	{ std::lock_guard<std::mutex> lk(ps2xmenu::m); a = ps2xmenu::cfg.aspect; }
	return a == 0 ? 1 : a == 2 ? 2 : 0;
}
// Das Spiel meldet seinen Bildmodus -> Ausgabe-Seitenverhaeltnis passend umschalten
PS2XGS_API void ps2xgs_game_wide(int wide)
{
	if (ps2xmenu::gameWide.exchange(wide != 0) != (wide != 0))
		ps2xmenu::applyPending.store(true);
}
// ---- Ton: SPU2-Chip aus PCSX2, angesteuert vom IOP des Ports (Register, DMA, Takt, Interrupts) ----
extern void (*g_ps2xSpuIrqHook)(int); // IopIrq.cpp (globaler Namensraum!)
namespace ps2xspu
{
	std::atomic<u32> irqs{0}; // Bit 0 SPU-IRQ (INUM 9), Bit 1 DMA4 fertig, Bit 2 DMA7 fertig
	std::atomic<u64> nDma{0}, nDmaHw{0}, nIrq[3];
	void irqHook(int which) { irqs.fetch_or(1u << which); nIrq[which].fetch_add(1); }
	bool hooked = false;
	bool ready() { return VMManager::HasValidVM(); }
	void ensure()
	{
		if (!hooked)
		{
			g_ps2xSpuIrqHook = &irqHook;
			hooked = true;
		}
	}
}
PS2XGS_API void ps2xspu_write(unsigned int addr, unsigned short value, unsigned int cycle)
{
	if (!ps2xspu::ready()) return;
	ps2xspu::ensure();
	psxRegs.cycle = cycle;
	SPU2write(addr, value);
	{
		// PS2X_SPULOG: Aktivitaet des Sound-Treibers (Registerschreibungen, Key-On) alle 5 s
		static const bool log = std::getenv("PS2X_SPULOG") != nullptr;
		if (log)
		{
			static u64 n = 0, kon = 0;
			static auto last = std::chrono::steady_clock::now();
			++n;
			const u32 off = addr & 0x3FF;
			if ((off == 0x1A0 || off == 0x1A2) && value) ++kon;
			const auto now = std::chrono::steady_clock::now();
			if (now - last > std::chrono::seconds(5))
			{
				last = now;
				Console.WriteLn(fmt::format("[SPU2] {} Registerschreibungen, {} Key-On, {} DMA ({} KB), IRQ spu/dma4/dma7 {}/{}/{}", n, kon,
					ps2xspu::nDma.load(), ps2xspu::nDmaHw.load() / 512, ps2xspu::nIrq[0].load(), ps2xspu::nIrq[1].load(), ps2xspu::nIrq[2].load()));
				for (int c = 0; c < 2; ++c)
				{
					const V_Core& k = Cores[c];
					std::string vs;
					for (int v = 0; v < 24; ++v)
					{
						const V_Voice& vo = k.Voices[v];
						if (vo.ADSR.Phase == 0) continue;
						int nz = 0;
						for (u32 i = 0; i < 0x1000; ++i) nz += _spu2mem[(vo.StartA + i) & 0xFFFFF] != 0;
						vs += fmt::format(" v{}[a={:x} nz={} n={:x} p={:x} vol={}/{} env={} g={}/{}]", v, vo.StartA, nz, vo.NextA, vo.Pitch,
							vo.Volume.Left.Value, vo.Volume.Right.Value, vo.ADSR.Value, k.VoiceGates[v].DryL, k.VoiceGates[v].DryR);
					}
					Console.WriteLn(fmt::format("[SPU2] Kern{}: irq={} irqa={:x} master={}/{} inp={}/{} ext={}/{} fx={} admactl={:x} dry snd/inp/ext {}/{} {}/{} {}/{}{}", c,
						k.IRQEnable, k.IRQA, k.MasterVol.Left.Value, k.MasterVol.Right.Value, k.InpVol.Left, k.InpVol.Right, k.ExtVol.Left, k.ExtVol.Right, k.FxEnable,
						k.AutoDMACtrl, k.DryGate.SndL, k.DryGate.SndR, k.DryGate.InpL, k.DryGate.InpR, k.DryGate.ExtL, k.DryGate.ExtR, vs));
				}
			}
		}
	}
}
PS2XGS_API unsigned short ps2xspu_read(unsigned int addr, unsigned int cycle)
{
	if (!ps2xspu::ready()) return 0;
	ps2xspu::ensure();
	psxRegs.cycle = cycle;
	const u16 r = SPU2read(addr);
	{
		// PS2X_SPUBUSYLOG: libsd-Belegtpruefung (ATTR-DMA-Bits 0x30, ADMAS) sichtbar machen
		static const bool bl = std::getenv("PS2X_SPUBUSYLOG") != nullptr;
		static int bn = 0;
		const u32 off = addr & 0x3FF;
		if (bl && bn < 400 && ((off == 0x19A && (r & 0x30)) || (off == 0x1B0 && r)))
		{
			++bn;
			Console.WriteLn(fmt::format("[SPU2-BELEGT] addr={:x} wert={:x}", addr, r));
		}
	}
	return r;
}
// DMA: core 0 = DMA4, 1 = DMA7; toSpu = 1 Speicher -> SPU2; halfwords = Anzahl 16-Bit-Woerter
PS2XGS_API void ps2xspu_dma(int core, unsigned short* mem, unsigned int halfwords, int toSpu, unsigned int cycle)
{
	if (!ps2xspu::ready()) { ps2xspu::irqs.fetch_or(core == 0 ? 2u : 4u); return; }
	ps2xspu::ensure();
	ps2xspu::nDma.fetch_add(1); ps2xspu::nDmaHw.fetch_add(halfwords);
	psxRegs.cycle = cycle;
	if (core == 0) HW_DMA4_CHCR |= 0x01000000; else HW_DMA7_CHCR |= 0x01000000;
	{
		static const bool dlog = std::getenv("PS2X_SPUDMALOG") != nullptr;
		static int dn = 0;
		if (dlog && dn < 400)
		{
			++dn;
			int nz = 0;
			for (u32 i = 0; i < halfwords; ++i) nz += mem[i] != 0;
			Console.WriteLn(fmt::format("[SPU2-DMA] kern{} {} tsa={:x} aktiv={:x} hw={} nz={} adma={:x} rest={}", core, toSpu ? "->SPU" : "<-SPU",
				Cores[core].TSA, Cores[core].ActiveTSA, halfwords, nz, Cores[core].AutoDMACtrl, Cores[core].ReadSize));
		}
	}
	if (toSpu)
	{
		if (core == 0) SPU2writeDMA4Mem(mem, halfwords); else SPU2writeDMA7Mem(mem, halfwords);
		// Normale (nicht-Auto-)DMA sofort vollstaendig in den SPU2-Speicher kopieren. PCSX2 kopiert sonst in 0x100-Wort-
		// Stuecken ueber die Transferzeit und liest dabei weiter aus dem IOP-Speicher. Der Port meldet die DMA dem Spiel
		// sofort als fertig (IOP-Zeit laeuft bei RPC-Bursts kaum); das Spiel ueberschreibt den Puffer dann sofort mit dem
		// naechsten Soundbank-Teil. Der Abschluss (IRQA-Pruefung, DMA-IRQ) laeuft beim naechsten SPU2-Takt wie gewohnt.
		V_Core& k = Cores[core];
		if (!k.AdmaInProgress)
		{
			for (int guard = 0; k.ReadSize && guard < 65536; ++guard)
				k.FinishDMAwrite();
			k.LastClock = psxRegs.cycle;
			k.DMAICounter = 1;
			k.Regs.STATX |= 0x80;
			k.Regs.STATX &= ~0x400;
		}
	}
	else
	{
		if (core == 0) SPU2readDMA4Mem(mem, halfwords); else SPU2readDMA7Mem(mem, halfwords);
	}
}
PS2XGS_API void ps2xspu_async(unsigned int cycle)
{
	if (!ps2xspu::ready()) return;
	ps2xspu::ensure();
	psxRegs.cycle = cycle;
	SPU2async();
}
PS2XGS_API unsigned int ps2xspu_take_irqs()
{
	return ps2xspu::irqs.exchange(0);
}

// Spiel-Optionen fuer den Port (id 0 = Kamera-Unschaerfe, 1 = TS2-Nachzieheffekt). Ohne aktives Menue (Testlaeufe): fallback.
PS2XGS_API int ps2xgs_option(int id, int fallback)
{
	if (!ps2xmenu::active.load())
		return fallback;
	std::lock_guard<std::mutex> lk(ps2xmenu::m);
	switch (id)
	{
		case 0: return ps2xmenu::cfg.camBlur;
		case 1: return ps2xmenu::cfg.nachzieh;
		case 2: return ps2xmenu::cfg.maus ? 1 : 0;
		case 3: return ps2xmenu::cfg.mausSens;
		case 4: return ps2xmenu::cfg.mausInvert ? 1 : 0;
		case 5: return ps2xmenu::cfg.mausZielmodus ? 1 : 0;
		case 6: return ps2xmenu::cfg.zoomSens;
		case 7: return ps2xin::zoomGehalten() ? 1 : 0;
		case 8: return ps2xhot::freecam.load() ? 1 : 0;
		default: return fallback;
	}
}
// Mauszielen: gesammelte relative Bewegung abholen. 0 = Maus nicht gefangen (Menue, kein Fokus, aus).
PS2XGS_API int ps2xgs_mouse(int* dx, int* dy)
{
	const int x = ps2xin::mausDX.exchange(0), y = ps2xin::mausDY.exchange(0);
	if (!ps2xin::mausGefangen.load() || ps2xmenu::open.load())
	{
		if (dx) *dx = 0; if (dy) *dy = 0;
		return 0;
	}
	if (x != 0 || y != 0) ps2xin::mausAktivMs.store(ps2xin::jetztMs());
	if (dx) *dx = x; if (dy) *dy = y;
	return 1;
}
// Online (TS2): Spiel hat die Netzwerk-Konfiguration erreicht -> Serverbrowser oeffnen
PS2XGS_API void ps2xgs_online_browser()
{
	ps2xmenu::online::oeffnen.store(true);
}
// Online (TS2): Verbindungsstatus der Spiel-Seite fuer den Serverbrowser
PS2XGS_API void ps2xgs_online_status(const char* text)
{
	std::lock_guard<std::mutex> lk(ps2xmenu::online::om);
	ps2xmenu::online::spielStatus = text ? text : "";
}
// Spielmenue offen? Der Port pausiert dann das Spiel (EE) am naechsten VBlank.
PS2XGS_API int ps2xgs_menu_open()
{
	return ps2xmenu::open.load() ? 1 : 0;
}
// Vibration vom Spiel: kleiner Motor (an/aus) und grosser Motor (0..255), wie DualShock 2
PS2XGS_API void ps2xgs_rumble(int small_on, int large)
{
	const u32 lo = small_on ? 0xFFFFu : 0u;
	const u32 hi = static_cast<u32>(std::clamp(large, 0, 255)) * 257u;
	ps2xin::rumble.store(lo | (hi << 16));
}

PS2XGS_API void ps2xgs_stop()
{
	Host::RequestVMShutdown(false, false, false);
	GSRunner::StopPlatformMessagePump();
	if (s_ps2x_cpu_thread.joinable())
		s_ps2x_cpu_thread.join();
	if (s_ps2x_pump_thread.joinable())
		s_ps2x_pump_thread.join();
	GSRunner::DestroyPlatformWindow();
}
