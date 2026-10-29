#include "BTForgeMcpServer.h"

#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"

namespace
{
	constexpr uint32 DefaultPort = 8090;

	FBTForgeMcpServer GMcpServer;

	uint32 ParsePortArg(const TArray<FString>& Args)
	{
		if (Args.Num() > 0)
		{
			const int32 Parsed = FCString::Atoi(*Args[0]);
			if (Parsed > 0 && Parsed < 65536)
			{
				return static_cast<uint32>(Parsed);
			}
		}
		return DefaultPort;
	}

	void ExecStartServer(const TArray<FString>& Args)
	{
		const uint32 Port = ParsePortArg(Args);
		if (GMcpServer.IsRunning())
		{
			if (GMcpServer.GetPort() == Port)
			{
				UE_LOG(LogBTForgeMcp, Display, TEXT("Server already running on port %u."), Port);
				return;
			}
			GMcpServer.Stop();
		}
		GMcpServer.Start(Port);
	}

	void ExecStopServer(const TArray<FString>& Args)
	{
		GMcpServer.Stop();
	}

	void ExecStatus(const TArray<FString>& Args)
	{
		if (GMcpServer.IsRunning())
		{
			UE_LOG(LogBTForgeMcp, Display, TEXT("BTForge MCP: RUNNING on http://127.0.0.1:%u/mcp"), GMcpServer.GetPort());
		}
		else
		{
			UE_LOG(LogBTForgeMcp, Display, TEXT("BTForge MCP: STOPPED"));
		}
	}
}

class FBTForgeMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		StartCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BTForge.MCP.StartServer"),
			TEXT("Start the BTForge MCP HTTP server. Optional argument: port (default 8090)."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&ExecStartServer),
			ECVF_Default);

		StopCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BTForge.MCP.StopServer"),
			TEXT("Stop the BTForge MCP HTTP server."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&ExecStopServer),
			ECVF_Default);

		StatusCommand = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("BTForge.MCP.Status"),
			TEXT("Report whether the BTForge MCP HTTP server is running."),
			FConsoleCommandWithArgsDelegate::CreateStatic(&ExecStatus),
			ECVF_Default);

		// Auto-start is opt-in per project: EditorPerProjectUserSettings.ini
		//   [BTForgeMCP]
		//   bAutoStartServer=True
		//   ServerPort=8090
		// EditorPerProjectUserSettings.ini is not guaranteed to be loaded into
		// GConfig during early module phases (observed on 5.5: module loads in
		// PreInit, the editor ini arrives later), so the read is deferred until
		// the engine is fully initialized - the same moment Epic's MCP server
		// starts on 5.8.
		FCoreDelegates::OnPostEngineInit.AddRaw(this, &FBTForgeMCPModule::MaybeAutoStart);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.RemoveAll(this);
		GMcpServer.Stop();
		if (StartCommand) { IConsoleManager::Get().UnregisterConsoleObject(StartCommand); StartCommand = nullptr; }
		if (StopCommand) { IConsoleManager::Get().UnregisterConsoleObject(StopCommand); StopCommand = nullptr; }
		if (StatusCommand) { IConsoleManager::Get().UnregisterConsoleObject(StatusCommand); StatusCommand = nullptr; }
	}

private:
	void MaybeAutoStart()
	{
		if (GMcpServer.IsRunning() || GConfig == nullptr)
		{
			return;
		}
		bool bAutoStart = false;
		int32 ConfigPort = static_cast<int32>(DefaultPort);
		GConfig->GetBool(TEXT("BTForgeMCP"), TEXT("bAutoStartServer"), bAutoStart, GEditorPerProjectIni);
		GConfig->GetInt(TEXT("BTForgeMCP"), TEXT("ServerPort"), ConfigPort, GEditorPerProjectIni);
		if (bAutoStart)
		{
			GMcpServer.Start(ConfigPort > 0 && ConfigPort < 65536 ? static_cast<uint32>(ConfigPort) : DefaultPort);
		}
	}

	IConsoleObject* StartCommand = nullptr;
	IConsoleObject* StopCommand = nullptr;
	IConsoleObject* StatusCommand = nullptr;
};

IMPLEMENT_MODULE(FBTForgeMCPModule, BTForgeMCP);
