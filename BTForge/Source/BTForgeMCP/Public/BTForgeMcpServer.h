// Self-contained MCP (Model Context Protocol) server for BTForge.
//
// Implements a stateless Streamable-HTTP MCP endpoint over the engine's
// HTTPServer module: JSON-RPC 2.0 with the initialize / ping / tools/list /
// tools/call methods. This is an original implementation written against the
// public MCP specification (modelcontextprotocol.io, Apache-2.0); it contains
// no Epic engine source code, so the plugin can be distributed freely on any
// engine version (5.4+). On 5.8+ the separate BTForgeOfficial plugin
// additionally exposes the same tools through Epic's ModelContextProtocol
// server on port 8000.

#pragma once

#include "CoreMinimal.h"
#include "HttpResultCallback.h"
#include "HttpRouteHandle.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBTForgeMcp, Log, All);

class IHttpRouter;

class FBTForgeMcpServer
{
public:
	~FBTForgeMcpServer();

	bool Start(uint32 InPort);
	void Stop();
	bool IsRunning() const { return PostRoute != nullptr; }
	uint32 GetPort() const { return Port; }

private:
	bool HandlePostRequest(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);
	bool HandleUnsupportedVerb(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle PostRoute = nullptr;
	FHttpRouteHandle GetRoute = nullptr;
	FHttpRouteHandle DeleteRoute = nullptr;
	uint32 Port = 0;
};
