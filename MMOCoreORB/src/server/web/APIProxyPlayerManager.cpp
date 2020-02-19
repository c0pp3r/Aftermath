/*
                Copyright <SWGEmu>
        See file COPYING for copying conditions.*/

/**
 * @author      : lordkator (lordkator@swgemu.com)
 * @file        : APIProxyPlayerManager.cpp
 * @created     : Sun Jan  5 11:26:18 UTC 2020
 */

#ifdef WITH_REST_API

#include "engine/engine.h"
#include "server/ServerCore.h"
#include "server/zone/ZoneServer.h"
#include "server/login/account/AccountManager.h"
#include "server/zone/managers/player/PlayerManager.h"
#include "server/zone/objects/player/PlayerObject.h"
#include "server/zone/objects/creature/CreatureObject.h"
#include "server/zone/objects/creature/credits/CreditObject.h"

#include "APIProxyPlayerManager.h"
#include "APIRequest.h"

namespace server {
 namespace web3 {

server::zone::managers::player::PlayerManager* APIProxyPlayerManager::getPlayerManager() {
	auto server = getZoneServer();

	if (server == nullptr) {
		return nullptr;
	}

	return server->getPlayerManager();
}

void APIProxyPlayerManager::lookupCharacter(APIRequest& apiRequest) {
	if (!apiRequest.isMethodGET()) {
		apiRequest.fail("Only supports GET");
		return;
	}

	auto qName = apiRequest.getQueryFieldString("name", false);
	auto qNames = apiRequest.getQueryFieldString("names", false);
	auto qRecursive = apiRequest.getQueryFieldBool("recursive", false, false);
	int qMaxDepth = apiRequest.getQueryFieldUnsignedLong("maxdepth", false, 1000);

	if (qName.isEmpty() && qNames.isEmpty()) {
		apiRequest.fail("Invalid request, must specify query parameter name or names");
		return;
	}

	auto mode = apiRequest.getPathFieldString("mode", true);

	Vector<String> names;

	if (!qName.isEmpty()) {
		names.add(qName);
	}

	if (!qNames.isEmpty()) {
		StringTokenizer nameList(qNames);
		nameList.setDelimeter(",");

		while (nameList.hasMoreTokens()) {
			String name;
			nameList.getStringToken(name);

			if (!name.isEmpty()) {
				names.add(name);
			}
		}
	}

	auto playerManager = getPlayerManager();

	if (playerManager == nullptr) {
		apiRequest.fail("Unable to get playerManager");
		return;
	}

	JSONSerializationType result, found, objects;

	for (auto name : names) {
		auto creo = playerManager->getPlayer(name);

		if (creo != nullptr) {
			found[name] = creo->getObjectID();
		} else {
			found[name] = 0;
		}

		if (mode == "find") {
			if (qRecursive) {
				creo->writeRecursiveJSON(objects, qMaxDepth);
			} else {
				Locker wLock(creo);

				creo->writeRecursiveJSON(objects, 1);

				auto ghost = creo->getPlayerObject();

				if (ghost != nullptr) {
					ReadLocker gLock(ghost);
					auto oidPath = new Vector<uint64>;
					oidPath->add(creo->getObjectID());
					ghost->writeRecursiveJSON(objects, 1, oidPath);
					delete oidPath;
				}

				auto crobj = creo->getCreditObject();

				if (crobj != nullptr) {
					ReadLocker crLock(crobj);
					auto oid = crobj->_getObjectID();
					JSONSerializationType jsonData;
					crobj->writeJSON(jsonData);
					jsonData["_depth"] = 1;
					jsonData["_oid"] = oid;
					jsonData["_className"] = crobj->_getClassName();
					jsonData["_oidPath"] = JSONSerializationType::array();
					jsonData["_oidPath"].push_back(creo->getObjectID());
					jsonData["_oidPath"].push_back(oid);
					objects[String::valueOf(oid)] = jsonData;
				}
			}
		}
	}

	result["characters"] = found;

	if (mode == "find") {
		result["objects"] = objects;
	}

	apiRequest.success(result);
}

void APIProxyPlayerManager::handle(APIRequest& apiRequest) {
	if (!apiRequest.isMethodPOST()) {
		apiRequest.fail("Only supports POST");
		return;
	}

	if (!apiRequest.parseRequestJSON())
		return;

	auto command = apiRequest.getRequestFieldString("command", true);

	if (command.isEmpty()) {
		apiRequest.fail("Invalid request, command is empty.");
		return;
	}

	if (command != "kick" && command != "ban") {
		apiRequest.fail("Invalid command: [" + command + "]");
		return;
	}

	uint64_t accountID = apiRequest.getPathFieldUnsignedLong("accountID", true);

	uint32_t galaxyID = apiRequest.getPathFieldUnsignedLong("galaxyID", false, 0ull);

	uint64_t characterID = apiRequest.getPathFieldUnsignedLong("characterID", false, 0ull);

	uint64_t adminID = apiRequest.getRequestFieldUnsignedLong("admin", true);

	if (adminID == 0) {
		apiRequest.fail("Invalid request, admin is 0");
		return;
	}

	auto reason = apiRequest.getRequestFieldString("reason", true);

	if (reason.isEmpty()) {
		apiRequest.fail("Invalid request, reason is empty.");
		return;
	}

	int expires = apiRequest.getRequestFieldUnsignedLong("expires", false, 0);

	auto playerManager = getPlayerManager();

	if (playerManager == nullptr) {
		apiRequest.fail("Unable to get playerManager");
		return;
	}

	auto adminName = playerManager->getPlayerName(adminID);

	if (adminName.isEmpty()) {
		apiRequest.fail("admin [" + String::valueOf(adminID) + "] not found");
		return;
	}

	JSONSerializationType result;

	result["request"] = apiRequest.getRequestJSON();
	result["admin_name"] = adminName;

	if (command == "kick") {
		if (galaxyID == 0 || characterID == 0) {
			apiRequest.fail("Currently account kick is not implemented");
			return;
		}

		auto characterName = playerManager->getPlayerName(characterID);

		if (characterName.isEmpty()) {
			apiRequest.fail("character [" + String::valueOf(characterID) + "] not found");
			return;
		}

		result["character_name"] = characterName;

		if (!playerManager->kickUser(characterName, adminName, reason, expires > 0 ? true : false)) {
			apiRequest.fail("kickUser failed");
			return;
		}
	} else if (command == "ban") {
		Reference<CreatureObject*> adminCreo = playerManager->getPlayer(adminName);

		if (adminCreo == nullptr) {
			apiRequest.fail("failed to get admin creature object");
			return;
		}

		Reference<PlayerObject*> adminGhost = adminCreo->getPlayerObject();

		if (adminGhost == nullptr) {
			apiRequest.fail("failed to get admin player object");
			return;
		}

		Reference<Account*> account = AccountManager::getAccount(accountID);

		if (account == nullptr) {
			apiRequest.fail("Account not found (accountID: " + String::valueOf(accountID) + ")");
			return;
		}

		result["username"] = account->getUsername();

		String banResult;

		if (galaxyID == 0 || characterID == 0) {
			banResult = playerManager->banAccount(adminGhost, account, expires, reason);
		} else {
			auto characterName = playerManager->getPlayerName(characterID);

			if (characterName.isEmpty()) {
				apiRequest.fail("character [" + String::valueOf(characterID) + "] not found");
				return;
			}

			result["character_name"] = characterName;

			banResult = playerManager->banCharacter(adminGhost, account, characterName, galaxyID, expires, reason);
		}

		result["ban_result"] = banResult;

		adminCreo->sendSystemMessage("API command ban: " + banResult);
	}

	apiRequest.success(result);
}
}
}

#endif // WITH_REST_API
