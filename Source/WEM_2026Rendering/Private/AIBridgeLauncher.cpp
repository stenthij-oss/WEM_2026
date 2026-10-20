// Copyright Epic Games, Inc. All Rights Reserved.

#include "AIBridgeLauncher.h"

#include "AIOverlay.h"
#include "AIOverlaySettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogAIBridge, Log, All);

#if PLATFORM_WINDOWS
namespace AIBridgeLauncher
{
	namespace
	{
		/** Held by a bridge while it runs (bridge.py), so a second one does not start; on channel N, with ".N" after it. */
		const TCHAR* BridgeMutexName = TEXT("WEM_2026.AIOverlay.Bridge");

		/** Inside the project folder. */
		const TCHAR* BridgeScript = TEXT("Tools/AIOverlay/run_bridge.bat");
		const TCHAR* BridgePython = TEXT("StreamDiffusion/.venv/Scripts/python.exe");

		/**
		 * The job the bridge runs in, with everything it starts (cmd, the venv's Python and the Python it hands over to).
		 * Closing it ends them all, and Windows closes it when the game ends, crashed or not.
		 */
		HANDLE BridgeJob = nullptr;

		/** With backslashes, as cmd and CreateProcess take them. */
		FString WindowsPath(FString Path)
		{
			FPaths::MakePlatformFilename(Path);
			return Path;
		}

		/** Whether a bridge runs already on the channel: one that holds the mutex, or one from before it, by its heartbeat. */
		bool IsBridgeRunning(const int32 Channel)
		{
			const FString MutexName = Channel == 0 ? FString(BridgeMutexName) : FString::Printf(TEXT("%s.%d"), BridgeMutexName, Channel);
			if (HANDLE Mutex = OpenMutexW(SYNCHRONIZE, false, *MutexName))
			{
				CloseHandle(Mutex);
				return true;
			}

			HANDLE Mapping = OpenFileMappingW(FILE_MAP_READ, false, *AIOverlay::GetSharedName(AIOverlay::OutputName, Channel));
			if (!Mapping)
			{
				return false;
			}
			bool bRunning = false;
			if (const void* View = MapViewOfFile(Mapping, FILE_MAP_READ, 0, 0, sizeof(AIOverlay::FSharedHeader)))
			{
				const volatile AIOverlay::FSharedHeader* Header = static_cast<const volatile AIOverlay::FSharedHeader*>(View);
				if (Header->Magic == AIOverlay::SharedMagic)
				{
					// A running bridge moves it every few milliseconds, even between cycles.
					const uint32 Before = Header->Heartbeat;
					FPlatformProcess::Sleep(0.5f);
					bRunning = Header->Heartbeat != Before;
				}
				UnmapViewOfFile(View);
			}
			CloseHandle(Mapping);
			return bRunning;
		}

		/** The project folder with the bridge in it: the settings' one, or else the first found up from the game's folder. */
		FString FindProjectFolder(const FString& Configured)
		{
			if (!Configured.IsEmpty())
			{
				FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), Configured);
				FPaths::NormalizeDirectoryName(Folder);
				return FPaths::FileExists(Folder / BridgeScript) ? Folder : FString();
			}

			// A game packaged into the project's Packaged folder finds the project two or three folders up.
			FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			FPaths::NormalizeDirectoryName(Folder);
			while (!Folder.IsEmpty())
			{
				if (FPaths::FileExists(Folder / BridgeScript))
				{
					return Folder;
				}
				const FString Parent = FPaths::GetPath(Folder);
				if (Parent == Folder)
				{
					break;
				}
				Folder = Parent;
			}
			return FString();
		}

		void Launch(const FString& ProjectFolder, const FString& Options)
		{
			const FString Script = FPaths::ConvertRelativePathToFull(ProjectFolder / BridgeScript);
			const FString LogFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectLogDir());
			const FString BridgeLogPath = LogFolder / TEXT("AIBridge.log");
			IFileManager::Get().MakeDirectory(*LogFolder, true);

			// The bridge's prints go to the log, as UTF-8 (a prompt may hold any letter) and as they come.
			SECURITY_ATTRIBUTES Inheritable = { sizeof(SECURITY_ATTRIBUTES), nullptr, true };
			HANDLE LogFile = CreateFileW(*WindowsPath(BridgeLogPath), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				&Inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			HANDLE Nothing = CreateFileW(TEXT("NUL"), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inheritable, OPEN_EXISTING, 0, nullptr);
			if (LogFile == INVALID_HANDLE_VALUE || Nothing == INVALID_HANDLE_VALUE)
			{
				UE_LOG(LogAIBridge, Warning, TEXT("Could not open %s (error %u); the bridge is not started."), *BridgeLogPath, GetLastError());
				if (LogFile != INVALID_HANDLE_VALUE) { CloseHandle(LogFile); }
				if (Nothing != INVALID_HANDLE_VALUE) { CloseHandle(Nothing); }
				return;
			}
			SetEnvironmentVariableW(TEXT("PYTHONIOENCODING"), TEXT("utf-8"));
			SetEnvironmentVariableW(TEXT("PYTHONUNBUFFERED"), TEXT("1"));

			// Only these two handles go to the bridge, none of the game's others.
			HANDLE Inherited[] = { LogFile, Nothing };
			SIZE_T AttributesSize = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &AttributesSize);
			TArray<uint8> AttributesMemory;
			AttributesMemory.SetNumZeroed(AttributesSize);
			LPPROC_THREAD_ATTRIBUTE_LIST Attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(AttributesMemory.GetData());
			InitializeProcThreadAttributeList(Attributes, 1, 0, &AttributesSize);
			UpdateProcThreadAttribute(Attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Inherited, sizeof(Inherited), nullptr, nullptr);

			STARTUPINFOEXW Startup = {};
			Startup.StartupInfo.cb = sizeof(Startup);
			Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			Startup.StartupInfo.hStdInput = Nothing;
			Startup.StartupInfo.hStdOutput = LogFile;
			Startup.StartupInfo.hStdError = LogFile;
			Startup.lpAttributeList = Attributes;

			// cmd /s takes everything between the outer quotes as it is: the script's quoted path and the options.
			FString Shell = FPlatformMisc::GetEnvironmentVariable(TEXT("ComSpec"));
			if (Shell.IsEmpty())
			{
				Shell = TEXT("C:\\Windows\\System32\\cmd.exe");
			}
			FString CommandLine = FString::Printf(TEXT("\"%s\" /d /s /c \"\"%s\" %s\""), *Shell, *WindowsPath(Script), *Options.TrimStartAndEnd());

			HANDLE Job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits = {};
			Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			const bool bJob = Job && SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits));

			PROCESS_INFORMATION Process = {};
			const bool bStarted = bJob && CreateProcessW(nullptr, CommandLine.GetCharArray().GetData(), nullptr, nullptr, true,
				CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, *WindowsPath(ProjectFolder),
				&Startup.StartupInfo, &Process);
			const DWORD Error = GetLastError();
			DeleteProcThreadAttributeList(Attributes);
			CloseHandle(LogFile);
			CloseHandle(Nothing);

			if (!bStarted)
			{
				UE_LOG(LogAIBridge, Warning, TEXT("Could not start the bridge (%s, error %u)."), *CommandLine, Error);
				if (Job) { CloseHandle(Job); }
				return;
			}
			// Into the job before it runs, so nothing it starts escapes it.
			if (!AssignProcessToJobObject(Job, Process.hProcess))
			{
				UE_LOG(LogAIBridge, Warning, TEXT("Could not tie the bridge to the game (error %u); it is not started."), GetLastError());
				TerminateProcess(Process.hProcess, 1);
				CloseHandle(Process.hThread);
				CloseHandle(Process.hProcess);
				CloseHandle(Job);
				return;
			}
			ResumeThread(Process.hThread);
			CloseHandle(Process.hThread);
			CloseHandle(Process.hProcess);
			BridgeJob = Job;

			UE_LOG(LogAIBridge, Log, TEXT("Started the bridge: %s. It prints to %s, and is ready once it says so, about a minute from now."), *CommandLine, *BridgeLogPath);
		}
	}
}
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace AIBridgeLauncher
{
	void Start()
	{
#if PLATFORM_WINDOWS
		if (BridgeJob || IsRunningCommandlet())
		{
			return;
		}
#if WITH_EDITOR
		// The editor's games (play in editor, and -game runs on its binaries) are often several at once, by hand or for
		// tests: there the bridge is started by hand.
		if (!FParse::Param(FCommandLine::Get(), TEXT("AIBridge")))
		{
			return;
		}
#endif
		const UAIOverlaySettings* Settings = GetDefault<UAIOverlaySettings>();
		if (!Settings->bStartBridge || FParse::Param(FCommandLine::Get(), TEXT("NoAIBridge")))
		{
			UE_LOG(LogAIBridge, Log, TEXT("Not starting the bridge (turned off in the settings, or -NoAIBridge)."));
			return;
		}
		// A game on another channel (-AIOverlayChannel=N, AIOverlay.h) has a bridge of its own.
		int32 Channel = 0;
		FParse::Value(FCommandLine::Get(), TEXT("AIOverlayChannel="), Channel);
		if (IsBridgeRunning(Channel))
		{
			UE_LOG(LogAIBridge, Log, TEXT("A bridge runs already; the game uses that one, and leaves it running."));
			return;
		}

		const FString ProjectFolder = FindProjectFolder(Settings->BridgeFolder.Path);
		if (ProjectFolder.IsEmpty())
		{
			UE_LOG(LogAIBridge, Warning, TEXT("Found no bridge (%s) %s; the AI overlay stays off. Set the project folder under Bridge on the AI Overlay settings page, or start the bridge by hand."),
				BridgeScript, Settings->BridgeFolder.Path.IsEmpty() ? *FString::Printf(TEXT("in the folders above %s"), *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())) : *FString::Printf(TEXT("in %s"), *Settings->BridgeFolder.Path));
			return;
		}
		if (!FPaths::FileExists(ProjectFolder / BridgePython))
		{
			UE_LOG(LogAIBridge, Warning, TEXT("The bridge's Python (%s) is missing; the AI overlay stays off. Structures/AI_OVERLAY.md says how to set it up."), *(ProjectFolder / BridgePython));
			return;
		}
		Launch(ProjectFolder, Channel == 0 ? Settings->BridgeOptions : FString::Printf(TEXT("--channel %d %s"), Channel, *Settings->BridgeOptions));
#endif
	}

	void Stop()
	{
#if PLATFORM_WINDOWS
		if (BridgeJob)
		{
			TerminateJobObject(BridgeJob, 0);
			CloseHandle(BridgeJob);
			BridgeJob = nullptr;
			UE_LOG(LogAIBridge, Log, TEXT("Stopped the bridge."));
		}
#endif
	}
}
