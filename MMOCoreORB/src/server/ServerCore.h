/*
				Copyright <SWGEmu>
		See file COPYING for copying conditions.*/

#ifndef SERVERCORE_H_
#define SERVERCORE_H_

#include "engine/engine.h"
#include "system/lang/Function.h"
#include "system/io/Pipe.h"

#include "server/features/Features.h"

namespace server {
	namespace zone{
		class ZoneServer;
	}
}

using namespace server::zone;

#include "server/login/LoginServer.h"
#ifdef WITH_SESSION_API
#include "server/login/SessionAPIClient.h"
#endif // WITH_SESSION_API
#include "server/ping/PingServer.h"

namespace conf {
	class ConfigManager;
}

using namespace conf;

class ServerDatabase;
class MantisDatabase;
class StatusServer;

namespace server {
 namespace web3 {
 	class RESTServer;
 }
}

namespace engine {
	namespace core {
		class MetricsManager;
	}
}

class ServerCore : public Core, public Logger {
	Pipe consoleCommandPipe;
	ConfigManager* configManager;
	ServerDatabase* database;
	MantisDatabase* mantisDatabase;
	DistributedObjectBroker* orb;
	Reference<server::login::LoginServer*> loginServer;
	Reference<StatusServer*> statusServer;
	server::features::Features* features;
	Reference<PingServer*> pingServer;
	MetricsManager* metricsManager;
	server::web3::RESTServer* restServer;
#ifdef WITH_SESSION_API
	Reference<server::login::SessionAPIClient*> sessionAPIClient;
#endif // WITH_SESSION_API

	Mutex shutdownBlockMutex;
	Condition waitCondition;

public:
	enum CommandResult {
		SUCCESS = 0,
		ERROR = 1,
		SHUTDOWN,
		NOTFOUND
	};

private:
	VectorMap<String, Function<CommandResult(const String& arguments)>> consoleCommands;

	bool handleCmds;

	static SortedVector<String> arguments;
	static ManagedReference<server::zone::ZoneServer*> zoneServerRef;
	static bool truncateAllData;
	static ServerCore* instance;

	void registerConsoleCommmands();
	CommandResult processConsoleCommand(const String& commandString);

public:
	ServerCore(bool truncateDatabases, const SortedVector<String>& args);
	~ServerCore();

	void initialize() override;
	void initializeCoreContext();

	void finalizeContext() override;

	void run() override;

	void shutdown();
	void queueConsoleCommand(const String& commandString);
	void handleCommands();
	void processConfig();
	void signalShutdown();

	// getters
	static server::zone::ZoneServer* getZoneServer() {
		return zoneServerRef.get();
	}

	static bool truncateDatabases() {
		return truncateAllData;
	}

	static ServerCore* getInstance() {
		return instance;
	}

	static Logger& logger() {
		return *instance;
	}

	static bool hasArgument(const String& arg) {
		return arguments.contains(arg);
	}

	static int getSchemaVersion();
};

#endif /*SERVERCORE_H_*/
