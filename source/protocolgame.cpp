////////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
////////////////////////////////////////////////////////////////////////
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
////////////////////////////////////////////////////////////////////////
#include "otpch.h"
#include <boost/function.hpp>
#include <iostream>
#include <iomanip>

#include "protocolgame.h"
#include "textlogger.h"

#include "waitlist.h"
#include "player.h"

#include "connection.h"
#include "networkmessage.h"
#include "outputmessage.h"

#include "iologindata.h"
#include "ioban.h"

#include "items.h"
#include "tile.h"
#include "house.h"

#include "actions.h"
#include "creatureevent.h"

#include "chat.h"
#include "configmanager.h"
#include "game.h"

extern Game g_game;
extern ConfigManager g_config;
extern Actions actions;
extern CreatureEvents* g_creatureEvents;
extern Chat g_chat;

template<class FunctionType>
void ProtocolGame::addGameTaskInternal(uint32_t delay, const FunctionType& func)
{
	if (delay > 0)
		Dispatcher::getInstance().addTask(createTask(delay, func));
	else
		Dispatcher::getInstance().addTask(createTask(func));
}

#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t ProtocolGame::protocolGameCount = 0;

#endif
void ProtocolGame::setPlayer(Player* p)
{
	player = p;
}

void ProtocolGame::releaseProtocol()
{
	if (player)
	{
		if (!m_spectator)
		{
			if (player->client->getOwner() == this)
				player->client->setOwner(NULL);
		}
		else if (player->client->isBroadcasting())
			player->client->removeSpectator(this);
	}

	//Protocol::releaseProtocol();
}

void ProtocolGame::deleteProtocolTask()
{
	if (player)
	{
		g_game.freeThing(player);
		player = NULL;
	}

	//Protocol::deleteProtocolTask();
}

void ProtocolGame::spectate(const std::string& name, const std::string& password)
{
	PlayerVector players = g_game.getPlayersByName(name);
	Player* _player = NULL;
	if (!players.empty())
		_player = players[random_range(0, (players.size() - 1))];

	if (!_player || _player->isRemoved() || !_player->client->isBroadcasting() || !_player->client->getOwner())
	{
		disconnectClient(0x14, "Stream unavailable.");
		return;
	}

	if (_player->client->banned(getIP()))
	{
		disconnectClient(0x14, "You are banned from this stream.");
		return;
	}

	if (!_player->client->check(password))
	{
		disconnectClient(0x14, "This stream is protected! Invalid password.");
		return;
	}

	m_spectator = true;
	player = _player;
	player->addRef();
	player->client->addSpectator(this);

	player->sendCreatureAppear(player, this);
	player->sendContainers(this);
	if (PrivateChatChannel * channel = g_chat.getPrivateChannel(player))
		chat(channel->getId());

	m_acceptPackets = true;
}

bool ProtocolGame::login(const std::string& name, uint32_t id, const std::string&,
	OperatingSystem_t operatingSystem, uint16_t version, bool gamemaster)
{
	//dispatcher thread
	PlayerVector players = g_game.getPlayersByName(name);
	Player* _player = NULL;
	if (!players.empty())
		_player = players[random_range(0, (players.size() - 1))];

	bool accountManager = g_config.getBool(ConfigManager::ACCOUNT_MANAGER);
	if (!_player || g_config.getNumber(ConfigManager::ALLOW_CLONES) ||
		(accountManager && name == "Account Manager"))
	{
		player = new Player(name, this);
		player->addRef();

		player->setID();
		if (!IOLoginData::getInstance()->loadPlayer(player, name, true))
		{
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		Ban ban;
		ban.value = player->getGUID();
		ban.param = PLAYERBAN_BANISHMENT;

		ban.type = BAN_PLAYER;
		if (IOBan::getInstance()->getData(ban) && !player->hasFlag(PlayerFlag_CannotBeBanned))
		{
			bool deletion = ban.expires < 0;
			std::string name_ = "Automatic ";
			if (!ban.adminId)
				name_ += (deletion ? "deletion" : "banishment");
			else
				IOLoginData::getInstance()->getNameByGuid(ban.adminId, name_, true);

			std::ostringstream stream;
			stream << "Your account has been " << (deletion ? "deleted" : "banished") << " at:\n" << formatDateEx(ban.added, "%d %b %Y").c_str() << " by: " << name_.c_str()
				<< "\nReason:\n" << getReason(ban.reason).c_str() << ".\nComment:\n" << ban.comment.c_str() << ".\nYour " << (deletion ? "account won't be undeleted" : "banishment will be lifted at:\n")
				<< (deletion ? "" : formatDateEx(ban.expires).c_str());
			disconnectClient(0x14, stream.str().c_str());
			return false;
		}

		if (IOBan::getInstance()->isPlayerBanished(player->getGUID(), PLAYERBAN_LOCK) && id != 1)
		{
			if (g_config.getBool(ConfigManager::NAMELOCK_MANAGER))
			{
				player->name = "Account Manager";
				player->accountManager = MANAGER_NAMELOCK;

				player->managerNumber = id;
				player->managerString2 = name;
			}
			else
			{
				disconnectClient(0x14, "Your character has been namelocked.");
				return false;
			}
		}
		else if (player->getName() == "Account Manager")
		{
			if (!g_config.getBool(ConfigManager::ACCOUNT_MANAGER))
			{
				disconnectClient(0x14, "Account Manager is disabled.");
				return false;
			}

			if (id != 1)
			{
				player->accountManager = MANAGER_ACCOUNT;
				player->managerNumber = id;
			}
			else
				player->accountManager = MANAGER_NEW;
		}

		if (gamemaster && !player->hasCustomFlag(PlayerCustomFlag_GamemasterPrivileges))
		{
			disconnectClient(0x14, "You are not a gamemaster! Turn off the gamemaster mode in your IP changer.");
			return false;
		}

		if (!player->hasFlag(PlayerFlag_CanAlwaysLogin))
		{
			if (g_game.getGameState() == GAMESTATE_CLOSING)
			{
				disconnectClient(0x14, "Gameworld is just going down, please come back later.");
				return false;
			}

			if (g_game.getGameState() == GAMESTATE_CLOSED)
			{
				disconnectClient(0x14, "Gameworld is currently closed, please come back later.");
				return false;
			}
		}

		if (g_config.getBool(ConfigManager::ONE_PLAYER_ON_ACCOUNT) && !player->isAccountManager() &&
			!IOLoginData::getInstance()->hasCustomFlag(id, PlayerCustomFlag_CanLoginMultipleCharacters))
		{
			bool found = false;
			PlayerVector tmp = g_game.getPlayersByAccount(id);
			for (PlayerVector::iterator it = tmp.begin(); it != tmp.end(); ++it)
			{
				if ((*it)->getName() != name)
					continue;

				found = true;
				break;
			}

			if (tmp.size() > 0 && !found)
			{
				disconnectClient(0x14, "You may only login with one character\nof your account at the same time.");
				return false;
			}
		}

		if (!WaitingList::getInstance()->login(player))
		{
			auto output = OutputMessagePool::getOutputMessage();

			std::ostringstream ss;
			ss << "Too many players online.\n" << "You are ";

			int32_t slot = WaitingList::getInstance()->getSlot(player);
			if (slot)
			{
				ss << "at ";
				if (slot > 0)
					ss << slot;
				else
					ss << "unknown";

				ss << " place on the waiting list.";
			}
			else
				ss << "awaiting connection...";

			output->addByte(0x16);
			output->addString(ss.str());
			output->addByte(WaitingList::getTime(slot));
			send(output);
			disconnect();
			return false;
		}

		if (!IOLoginData::getInstance()->loadPlayer(player, name))
		{
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		if (!g_game.placeCreature(player, player->getLoginPosition()))
		{
			Position pos = g_game.getClosestFreeTile(player, player->getMasterPosition(), true, false);
			if (!pos.x)
				pos = player->getMasterPosition();

			if (!g_game.placeCreature(player, pos, false, true))
			{
				disconnectClient(0x14, "Temple position is wrong. Contact with the administration.");
				return false;
			}
		}

		player->setClientVersion(version);
		player->setOperatingSystem(operatingSystem);

		if (player->isUsingOtclient())
		{
			player->registerCreatureEvent("ExtendedOpcode");
		}

		player->lastIP = player->getIP();
		player->lastLoad = OTSYS_TIME();
		player->lastLogin = std::max(time(NULL), player->lastLogin + 1);

		m_acceptPackets = true;
		return true;
	}

	if (gamemaster && !_player->hasCustomFlag(PlayerCustomFlag_GamemasterPrivileges))
	{
		disconnectClient(0x14, "You are not allowed to play on spectator mode.");
		return false;
	}

	if (_player->hasClient())
	{
		if (m_eventConnect || !g_config.getBool(ConfigManager::REPLACE_KICK_ON_LOGIN))
		{
			// task has already been scheduled just bail out (should not be overriden)
			disconnectClient(0x14, "You are already logged in.");
			return false;
		}

		_player->client->disconnect();
		_player->isConnecting = true;
		_player->setClientVersion(version);


		m_eventConnect = Scheduler::getInstance().addEvent(createSchedulerTask(
			1000, boost::bind(&ProtocolGame::connect, this, _player->getID(), operatingSystem, version)));
		return true;
	}


	return connect(_player->getID(), operatingSystem, version);
}

bool ProtocolGame::logout(bool displayEffect, bool forceLogout)
{
	//dispatcher thread
	if (!player)
		return false;

	if (player->hasCondition(CONDITION_EXHAUST, EXHAUST_DEFAULT))
	{
		player->sendTextMessage(MSG_STATUS_SMALL, "You have to wait a while.");
		return false;
	}

	if (!player->isRemoved())
	{
		if (!forceLogout)
		{
			if (!IOLoginData::getInstance()->hasCustomFlag(player->getAccount(), PlayerCustomFlag_CanLogoutAnytime))
			{
				if (player->getTile()->hasFlag(TILESTATE_NOLOGOUT))
				{
					if (Condition * condition = Condition::createCondition(CONDITIONID_DEFAULT, CONDITION_EXHAUST, 500, 0, false, EXHAUST_DEFAULT))
						player->addCondition(condition);

					player->sendCancelMessage(RET_YOUCANNOTLOGOUTHERE);
					return false;
				}

				if (player->hasCondition(CONDITION_INFIGHT))
				{
					player->sendCancelMessage(RET_YOUMAYNOTLOGOUTDURINGAFIGHT);
					return false;
				}

				if (!g_creatureEvents->playerLogout(player, false)) //let the script handle the error message
					return false;
			}
			else
				g_creatureEvents->playerLogout(player, true);
		}
		else if (!g_creatureEvents->playerLogout(player, true))
			return false;

		if (displayEffect && !player->isGhost())
			g_game.addMagicEffect(player->getPosition(), MAGIC_EFFECT_POFF);
	}

	player->client->clear(true);
	disconnect();
	if (player->isRemoved())
		return true;

	return g_game.removeCreature(player);
}

void ProtocolGame::chat(uint16_t channelId)
{
	PrivateChatChannel* tmp = g_chat.getPrivateChannel(player);
	if (!tmp)
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	if (channelId)
	{
		msg->addByte(0xB2);
		msg->add<uint16_t>(tmp->getId());
		msg->addString(tmp->getName());
	}
	else
	{
		msg->addByte(0xAB);
		msg->addByte(1);
		msg->add<uint16_t>(tmp->getId());
		msg->addString("Live Channel");
	}
}

bool ProtocolGame::connect(uint32_t playerId, OperatingSystem_t operatingSystem, uint16_t version)
{

	m_eventConnect = 0;

	Player* _player = g_game.getPlayerByID(playerId);
	if (!_player || _player->isRemoved() || _player->hasClient())
	{
		disconnectClient(0x14, "You are already logged in.");
		return false;
	}

	player = _player;
	player->addRef();
	player->client->setOwner(this);
	player->isConnecting = false;

	player->sendCreatureAppear(player, this);
	player->setOperatingSystem(operatingSystem);
	player->setClientVersion(version);

	player->lastIP = player->getIP();
	player->lastLoad = OTSYS_TIME();

	m_acceptPackets = true;
	return true;
}

void ProtocolGame::disconnect()
{
	if (Connection_ptr connection = getConnection())
		connection->close();
}

void ProtocolGame::disconnectClient(uint8_t error, const char* message)
{
	auto output = OutputMessagePool::getOutputMessage();
	output->addByte(error);
	output->addString(message);
	send(output);
	disconnect();
}

void ProtocolGame::onConnect() {}

void ProtocolGame::onRecvFirstMessage(NetworkMessage& msg)
{
	if (g_game.getGameState() == GAMESTATE_SHUTDOWN)
	{
		getConnection()->close();
		return;
	}

	OperatingSystem_t operatingSystem = (OperatingSystem_t)msg.get<uint16_t>();
	uint16_t version = msg.get<uint16_t>();
#ifdef _MULTIPLATFORM77
	if (!RSA_decrypt(msg))
	{
		disconnect();
		return;
	}

	uint32_t key[4] = { msg.get<uint32_t>(), msg.get<uint32_t>(), msg.get<uint32_t>(), msg.get<uint32_t>() };
	enableXTEAEncryption();
	setXTEAKey(key);
#endif

	if (operatingSystem >= CLIENTOS_OTCLIENT_LINUX)
		sendExtendedOpcode(0x00, std::string());

	bool gamemaster = (msg.get<char>() != (char)0);
	uint32_t name = msg.get<uint32_t>();
	std::string character = msg.getString(), password = msg.getString();
	if (!IOLoginData::getInstance()->playerExists(character))
	{
		disconnectClient(0x14, "This character does not exist.");
		return;
	}

	if (!g_config.getBool(ConfigManager::MANUAL_ADVANCED_CONFIG))
	{
		if (version < g_config.getNumber(ConfigManager::VERSION_MIN) || version > g_config.getNumber(ConfigManager::VERSION_MAX))
		{
			disconnectClient(0x14, g_config.getString(ConfigManager::VERSION_MSG).c_str());
			return;
		}
	}
	else
	{
		if (version < CLIENT_VERSION_MIN || version > CLIENT_VERSION_MAX)
		{
			disconnectClient(0x14, "Only clients with protocol " CLIENT_VERSION_STRING " allowed!");
			return;
		}
	}

	if (!name)
	{
		name = 10;
	}

	if (g_game.getGameState() < GAMESTATE_NORMAL)
	{
		disconnectClient(0x14, "Gameworld is just starting up, please wait.");
		return;
	}

	if (g_game.getGameState() == GAMESTATE_MAINTAIN)
	{
		disconnectClient(0x14, "Gameworld is under maintenance, please re-connect in a while.");
		return;
	}

	if (IOBan::getInstance()->isIpBanished(getIP()))
	{
		disconnectClient(0x14, "Your IP is banished!");
		return;
	}

	uint32_t id = 1;
	if (name != 10 && (!IOLoginData::getInstance()->getAccountId(name, id)))
	{
		disconnectClient(0x14, "Invalid account name.");
		return;
	}

	std::string hash, salt;
	if (name != 10 && (!IOLoginData::getInstance()->getPassword(id, hash, salt, character) || !encryptTest(salt + password, hash)))
	{

		disconnectClient(0x14, "Invalid password.");
		return;
	}

	Ban ban;
	ban.value = id;

	ban.type = BAN_ACCOUNT;
	if (IOBan::getInstance()->getData(ban) && !IOLoginData::getInstance()->hasFlag(id, PlayerFlag_CannotBeBanned))
	{
		bool deletion = ban.expires < 0;
		std::string name_ = "Automatic ";
		if (!ban.adminId)
			name_ += (deletion ? "deletion" : "banishment");
		else
			IOLoginData::getInstance()->getNameByGuid(ban.adminId, name_, true);

		std::ostringstream stream;
		stream << "Your account has been " << (deletion ? "deleted" : "banished") << " at:\n" << formatDateEx(ban.added, "%d %b %Y").c_str() << " by: " << name_.c_str()
			<< ".\nThe comment given was:\n" << ban.comment.c_str() << ".\nYour " << (deletion ? "account won't be undeleted" : "banishment will be lifted at:\n")
			<< (deletion ? "" : formatDateEx(ban.expires).c_str()) << ".";

		disconnectClient(0x14, stream.str().c_str());
		return;
	}

	if (name != 10)
		Dispatcher::getInstance().addTask(createTask(boost::bind(
			&ProtocolGame::login, this, character, id, password, operatingSystem, version, gamemaster)));
	else
		Dispatcher::getInstance().addTask(createTask(boost::bind(
			&ProtocolGame::spectate, this, character, password)));
}

void ProtocolGame::parsePacket(NetworkMessage& msg)
{
	if (!player || !m_acceptPackets || g_game.getGameState() == GAMESTATE_SHUTDOWN || !msg.getLength())
		return;

	uint8_t recvbyte = msg.get<char>();
	if (player->isRemoved() && recvbyte != 0x14) //a dead player cannot performs actions
		return;

	if (m_spectator)
	{
		switch (recvbyte)
		{
		case 0x14: parseLogout(msg); break;
		case 0x96: parseSay(msg); break;
		case 0x1E: parseReceivePing(msg); break;
		case 0x97: parseGetChannels(msg); break;
		case 0x98: parseOpenChannel(msg); break;
		case 0xC9: parseUpdateTile(msg); break;
		case 0xCA: parseUpdateContainer(msg); break;
		case 0xE8: parseDebugAssert(msg); break;
		case 0xA1: parseCancelTarget(msg); break;

		default:
			parseCancelWalk(msg);
			break;
		}
	}
	else if (player->isAccountManager())
	{
		switch (recvbyte)
		{
		case 0x14: parseLogout(msg); break;
		case 0x96: parseSay(msg); break;
		case 0x1E: parseReceivePing(msg); break;
		case 0xC9: parseUpdateTile(msg); break;
		case 0xE8: parseDebugAssert(msg); break;
		case 0xA1: parseCancelTarget(msg); break;

		default:
			parseCancelWalk(msg);
			break;
		}
	}
	else
	{
		switch (recvbyte)
		{
		case 0x14: parseLogout(msg); break;
		case 0x1E: parseReceivePing(msg); break;
		case 0x32: parseExtendedOpcode(msg); break;
		case 0x64: parseAutoWalk(msg); break;
		case 0x65:
		case 0x66:
		case 0x67:
		case 0x68: parseMove(msg, (Direction)(recvbyte - 0x65)); break;
		case 0x69: addGameTask(&Game::playerStopAutoWalk, player->getID()); break;
		case 0x6A: parseMove(msg, NORTHEAST); break;
		case 0x6B: parseMove(msg, SOUTHEAST); break;
		case 0x6C: parseMove(msg, SOUTHWEST); break;
		case 0x6D: parseMove(msg, NORTHWEST); break;
		case 0x6F:
		case 0x70:
		case 0x71:
		case 0x72: parseTurn(msg, (Direction)(recvbyte - 0x6F)); break;
		case 0x78: parseThrow(msg); break;
		case 0x7D: parseRequestTrade(msg); break;
		case 0x7E: parseLookInTrade(msg); break;
		case 0x7F: parseAcceptTrade(msg); break;
		case 0x80: parseCloseTrade(); break;
		case 0x82: parseUseItem(msg); break;
		case 0x83: parseUseItemEx(msg); break;
		case 0x84: parseBattleWindow(msg); break;
		case 0x85: parseRotateItem(msg); break;
		case 0x87: parseCloseContainer(msg); break;
		case 0x88: parseUpArrowContainer(msg); break;
		case 0x89: parseTextWindow(msg); break;
		case 0x8A: parseHouseWindow(msg); break;
		case 0x8C: parseLookAt(msg); break;
		case 0x8D: parseLookInBattleList(msg); break;
		case 0x96: parseSay(msg); break;
		case 0x97: parseGetChannels(msg); break;
		case 0x98: parseOpenChannel(msg); break;
		case 0x99: parseCloseChannel(msg); break;
		case 0x9A: parseOpenPrivate(msg); break;
		case 0x9B: parseProcessRuleViolation(msg); break;
		case 0x9C: parseCloseRuleViolation(msg); break;
		case 0x9D: parseCancelRuleViolation(msg); break;
		case 0xA0: parseFightModes(msg); break;
		case 0xA1: parseAttack(msg); break;
		case 0xA2: parseFollow(msg); break;
		case 0xA3: parseInviteToParty(msg); break;
		case 0xA4: parseJoinParty(msg); break;
		case 0xA5: parseRevokePartyInvite(msg); break;
		case 0xA6: parsePassPartyLeadership(msg); break;
		case 0xA7: parseLeaveParty(msg); break;
		case 0xAA: parseCreatePrivateChannel(msg); break;
		case 0xAB: parseChannelInvite(msg); break;
		case 0xAC: parseChannelExclude(msg); break;
		case 0xBE: parseCancelMove(msg); break;
		case 0xC9: parseUpdateTile(msg); break;
		case 0xCA: parseUpdateContainer(msg); break;
		case 0xD2:
			if ((!player->hasCustomFlag(PlayerCustomFlag_GamemasterPrivileges) || !g_config.getBool(
				ConfigManager::DISABLE_OUTFITS_PRIVILEGED)) && (g_config.getBool(ConfigManager::ALLOW_CHANGEOUTFIT)
					|| g_config.getBool(ConfigManager::ALLOW_CHANGECOLORS)))
				parseRequestOutfit(msg);
			break;
		case 0xD3:
			if ((!player->hasCustomFlag(PlayerCustomFlag_GamemasterPrivileges) || !g_config.getBool(ConfigManager::DISABLE_OUTFITS_PRIVILEGED))
				&& (g_config.getBool(ConfigManager::ALLOW_CHANGECOLORS) || g_config.getBool(ConfigManager::ALLOW_CHANGEOUTFIT)))
				parseSetOutfit(msg);
			break;
		case 0xDC: parseAddVip(msg); break;
		case 0xDD: parseRemoveVip(msg); break;
		case 0xE6: parseBugReport(msg); break;
		case 0xE7: parseViolationWindow(msg); break;
		case 0xE8: parseDebugAssert(msg); break;

		default:
		{
			if (g_config.getBool(ConfigManager::BAN_UNKNOWN_BYTES))
			{
				int64_t banTime = -1;
				ViolationAction_t action = ACTION_BANISHMENT;
				Account tmp = IOLoginData::getInstance()->loadAccount(player->getAccount(), true);

				tmp.warnings++;
				if (tmp.warnings >= g_config.getNumber(ConfigManager::WARNINGS_TO_DELETION))
					action = ACTION_DELETION;
				else if (tmp.warnings >= g_config.getNumber(ConfigManager::WARNINGS_TO_FINALBAN))
				{
					banTime = time(NULL) + g_config.getNumber(ConfigManager::FINALBAN_LENGTH);
					action = ACTION_BANFINAL;
				}
				else
					banTime = time(NULL) + g_config.getNumber(ConfigManager::BAN_LENGTH);

				if (IOBan::getInstance()->addAccountBanishment(tmp.number, banTime, 13, action,
					"Sending unknown packets to the server.", 0, player->getGUID()))
				{
					IOLoginData::getInstance()->saveAccount(tmp);
					player->sendTextMessage(MSG_INFO_DESCR, "You have been banished.");

					g_game.addMagicEffect(player->getPosition(), MAGIC_EFFECT_WRAPS_GREEN);
					Scheduler::getInstance().addEvent(createSchedulerTask(1000, boost::bind(
						&Game::kickPlayer, &g_game, player->getID(), false)));
				}
			}

			std::stringstream s;
			s << "Sent unknown byte: 0x" << std::hex << (int16_t)recvbyte << std::dec;
			Logger::getInstance()->eFile("bots/" + player->getName() + ".log", s.str(), true);
			break;
		}
		}
	}
}

void ProtocolGame::GetTileDescription(const Tile* tile, OutputMessage_ptr msg)
{
	int32_t count = 0;
	if (tile->ground)
	{
		msg->addItem(tile->ground);
		++count;
	}

	const TileItemVector* items = tile->getItemList();
	const CreatureVector* creatures = tile->getCreatures();

	ItemVector::const_iterator it;
	if (items)
	{
		for (it = items->getBeginTopItem(); (it != items->getEndTopItem() && count < 10); ++it, ++count)
			msg->addItem(*it);
	}

	if (creatures)
	{
		for (CreatureVector::const_iterator cit = creatures->begin(); (cit != creatures->end() && count < 10); ++cit)
		{
			if (!player->canSeeCreature(*cit))
				continue;

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown((*cit)->getID(), known, removedKnown);

			AddCreature(msg, (*cit), known, removedKnown);
			++count;
		}
	}

	if (items)
	{
		for (it = items->getBeginDownItem(); (it != items->getEndDownItem() && count < 10); ++it, ++count)
			msg->addItem(*it);
	}
}

void ProtocolGame::GetMapDescription(int32_t x, int32_t y, int32_t z,
	int32_t width, int32_t height, OutputMessage_ptr msg)
{
	int32_t skip = -1, startz, endz, zstep = 0;
	if (z > 7)
	{
		startz = z - 2;
		endz = std::min((int32_t)MAP_MAX_LAYERS - 1, z + 2);
		zstep = 1;
	}
	else
	{
		startz = 7;
		endz = 0;
		zstep = -1;
	}

	for (int32_t nz = startz; nz != endz + zstep; nz += zstep)
		GetFloorDescription(msg, x, y, nz, width, height, z - nz, skip);

	if (skip >= 0)
	{
		msg->addByte(skip);
		msg->addByte(0xFF);
		//cc += skip;
	}
}

void ProtocolGame::GetFloorDescription(OutputMessage_ptr msg, int32_t x, int32_t y, int32_t z,
	int32_t width, int32_t height, int32_t offset, int32_t& skip)
{
	Tile* tile = NULL;
	for (int32_t nx = 0; nx < width; ++nx)
	{
		for (int32_t ny = 0; ny < height; ++ny)
		{
			if ((tile = g_game.getTile(Position(x + nx + offset, y + ny + offset, z))))
			{
				if (skip >= 0)
				{
					msg->addByte(skip);
					msg->addByte(0xFF);
				}

				skip = 0;
				GetTileDescription(tile, msg);
			}
			else if (++skip == 0xFF)
			{
				msg->addByte(0xFF);
				msg->addByte(0xFF);
				skip = -1;
			}
		}
	}
}

void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown)
{
	// loop through the known creature list and check if the given creature is in
	for (std::list<uint32_t>::iterator it = knownCreatureList.begin(); it != knownCreatureList.end(); ++it)
	{
		if ((*it) != id)
			continue;

		// know... make the creature even more known...
		knownCreatureList.erase(it);
		knownCreatureList.push_back(id);

		known = true;
		return;
	}

	// ok, he is unknown...
	known = false;
	// ... but not in future
	knownCreatureList.push_back(id);
	// too many known creatures?
	if (knownCreatureList.size() > 150)
	{
		// lets try to remove one from the end of the list
		Creature* c = NULL;
		for (int16_t n = 0; n < 150; ++n)
		{
			removedKnown = knownCreatureList.front();
			if (!(c = g_game.getCreatureByID(removedKnown)) || !canSee(c))
				break;

			// this creature we can't remove, still in sight, so back to the end
			knownCreatureList.pop_front();
			knownCreatureList.push_back(removedKnown);
		}

		// hopefully we found someone to remove :S, we got only 150 tries
		// if not... lets kick some players with debug errors :)
		knownCreatureList.pop_front();
	}
	else // we can cache without problems :)
		removedKnown = 0;
}

bool ProtocolGame::canSee(const Creature* c) const
{
	return !c->isRemoved() && player->canSeeCreature(c) && canSee(c->getPosition());
}

bool ProtocolGame::canSee(const Position& pos) const
{
	return canSee(pos.x, pos.y, pos.z);
}

bool ProtocolGame::canSee(uint16_t x, uint16_t y, uint16_t z) const
{
	const Position& myPos = player->getPosition();
	if (myPos.z <= 7)
	{
		//we are on ground level or above (7 -> 0), view is from 7 -> 0
		if (z > 7)
			return false;
	}
	else if (myPos.z >= 8 && std::abs(myPos.z - z) > 2) //we are underground (8 -> 15), view is +/- 2 from the floor we stand on
		return false;

	//negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.z - z;
	return ((x >= myPos.x - 8 + offsetz) && (x <= myPos.x + 9 + offsetz) &&
		(y >= myPos.y - 6 + offsetz) && (y <= myPos.y + 7 + offsetz));
}

//********************** Parse methods *******************************//
void ProtocolGame::parseLogout(NetworkMessage&)
{
	if (m_spectator)
		Dispatcher::getInstance().addTask(createTask(boost::bind(&ProtocolGame::disconnect, this)));
	else
		Dispatcher::getInstance().addTask(createTask(boost::bind(&ProtocolGame::logout, this, true, false)));
}

void ProtocolGame::parseCancelWalk(NetworkMessage&)
{
	Dispatcher::getInstance().addTask(createTask(boost::bind(&ProtocolGame::sendCancelWalk, this)));
}

void ProtocolGame::parseCancelTarget(NetworkMessage&)
{
	Dispatcher::getInstance().addTask(createTask(boost::bind(&ProtocolGame::sendCancelTarget, this)));
}

void ProtocolGame::parseCreatePrivateChannel(NetworkMessage&)
{
	addGameTask(&Game::playerCreatePrivateChannel, player->getID());
}

void ProtocolGame::parseChannelInvite(NetworkMessage& msg)
{
	const std::string name = msg.getString();
	addGameTask(&Game::playerChannelInvite, player->getID(), name);
}

void ProtocolGame::parseChannelExclude(NetworkMessage& msg)
{
	const std::string name = msg.getString();
	addGameTask(&Game::playerChannelExclude, player->getID(), name);
}

void ProtocolGame::parseGetChannels(NetworkMessage&)
{
	if (m_spectator)
		Dispatcher::getInstance().addTask(createTask(boost::bind(&ProtocolGame::chat, this, 0)));
	else
		addGameTask(&Game::playerRequestChannels, player->getID());
}

void ProtocolGame::parseOpenChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.get<uint16_t>();
	if (m_spectator)
		Dispatcher::getInstance().addTask(createTask(boost::bind(&ProtocolGame::chat, this, channelId)));
	else
		addGameTask(&Game::playerOpenChannel, player->getID(), channelId);
}

void ProtocolGame::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.get<uint16_t>();
	addGameTask(&Game::playerCloseChannel, player->getID(), channelId);
}

void ProtocolGame::parseOpenPrivate(NetworkMessage& msg)
{
	const std::string receiver = msg.getString();
	addGameTask(&Game::playerOpenPrivateChannel, player->getID(), receiver);
}

void ProtocolGame::parseProcessRuleViolation(NetworkMessage& msg)
{
	const std::string reporter = msg.getString();
	addGameTask(&Game::playerProcessRuleViolation, player->getID(), reporter);
}

void ProtocolGame::parseCloseRuleViolation(NetworkMessage& msg)
{
	const std::string reporter = msg.getString();
	addGameTask(&Game::playerCloseRuleViolation, player->getID(), reporter);
}

void ProtocolGame::parseCancelRuleViolation(NetworkMessage&)
{
	addGameTask(&Game::playerCancelRuleViolation, player->getID());
}

void ProtocolGame::parseViolationWindow(NetworkMessage& msg)
{
	uint16_t statementId = 0;
	std::string target = msg.getString();
	uint8_t reason = msg.get<char>();
#ifdef _MULTIPLATFORM77
	ViolationAction_t action = (ViolationAction_t)msg.get<char>();
	std::string comment = msg.getString();
	statementId = msg.get<uint16_t>();
	/*channelId = */msg.get<uint16_t>();
#else
	std::string comment = msg.getString();
	ViolationAction_t action = (ViolationAction_t)msg.get<char>();
#endif
	bool ipBanishment = (msg.get<char>() == 0x01);
	addGameTask(&Game::playerViolationWindow, player->getID(), target,
		reason, action, comment, "", statementId, ipBanishment);
}

void ProtocolGame::parseCancelMove(NetworkMessage&)
{
	addGameTask(&Game::playerCancelAttackAndFollow, player->getID());
}

void ProtocolGame::parseReceivePing(NetworkMessage&)
{
	addGameTask(&Game::playerReceivePing, player->getID());
}

void ProtocolGame::parseAutoWalk(NetworkMessage& msg)
{
	uint8_t dirCount = msg.get<char>();
	if (dirCount > 128) //client limit
	{
		for (uint8_t i = 0; i < dirCount; ++i)
			msg.get<char>();

		std::ostringstream s;
		s << "Attempt to auto walk for " << (uint16_t)dirCount << " steps - client is limited to 128 steps.";
		Logger::getInstance()->eFile("bots/" + player->getName() + ".log", s.str(), true);
		return;
	}

	std::list<Direction> path;
	for (uint8_t i = 0; i < dirCount; ++i)
	{
		Direction dir = SOUTH;
		switch (msg.get<char>())
		{
		case 1:
			dir = EAST;
			break;
		case 2:
			dir = NORTHEAST;
			break;
		case 3:
			dir = NORTH;
			break;
		case 4:
			dir = NORTHWEST;
			break;
		case 5:
			dir = WEST;
			break;
		case 6:
			dir = SOUTHWEST;
			break;
		case 7:
			dir = SOUTH;
			break;
		case 8:
			dir = SOUTHEAST;
			break;
		default:
			continue;
		}

		path.push_back(dir);
	}

	addGameTask(&Game::playerAutoWalk, player->getID(), path);
}

void ProtocolGame::parseMove(NetworkMessage&, Direction dir)
{
	addGameTask(&Game::playerMove, player->getID(), dir);
}

void ProtocolGame::parseTurn(NetworkMessage&, Direction dir)
{
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), dir);
}

void ProtocolGame::parseRequestOutfit(NetworkMessage&)
{
	addGameTask(&Game::playerRequestOutfit, player->getID());
}

void ProtocolGame::parseSetOutfit(NetworkMessage& msg)
{
	Outfit_t newOutfit = player->defaultOutfit;
	if (g_config.getBool(ConfigManager::ALLOW_CHANGEOUTFIT))
#ifdef _MULTIPLATFORM77
		newOutfit.lookType = msg.get<uint16_t>();
#else
		newOutfit.lookType = msg.get<char>();
#endif
	else
		msg.skipBytes(2);

	if (g_config.getBool(ConfigManager::ALLOW_CHANGECOLORS))
	{
		newOutfit.lookHead = msg.get<char>();
		newOutfit.lookBody = msg.get<char>();
		newOutfit.lookLegs = msg.get<char>();
		newOutfit.lookFeet = msg.get<char>();
	}
	else
		msg.skipBytes(4);

	addGameTask(&Game::playerChangeOutfit, player->getID(), newOutfit);
}

void ProtocolGame::parseUseItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	int16_t stackpos = msg.get<char>();
	uint8_t index = msg.get<char>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItem, player->getID(), pos, stackpos, index, spriteId);
}

void ProtocolGame::parseUseItemEx(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t fromSpriteId = msg.get<uint16_t>();
	int16_t fromStackpos = msg.get<char>();
	Position toPos = msg.getPosition();
	uint16_t toSpriteId = msg.get<uint16_t>();
	int16_t toStackpos = msg.get<char>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItemEx, player->getID(),
		fromPos, fromStackpos, fromSpriteId, toPos, toStackpos, toSpriteId);
}

void ProtocolGame::parseBattleWindow(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	int16_t fromStackpos = msg.get<char>();
	uint32_t creatureId = msg.get<uint32_t>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseBattleWindow, player->getID(), fromPos, fromStackpos, creatureId, spriteId);
}

void ProtocolGame::parseCloseContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.get<char>();
	addGameTask(&Game::playerCloseContainer, player->getID(), cid);
}

void ProtocolGame::parseUpArrowContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.get<char>();
	addGameTask(&Game::playerMoveUpContainer, player->getID(), cid);
}

void ProtocolGame::parseUpdateTile(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	addGameTask(&Game::playerUpdateTile, player->getID(), pos);
}

void ProtocolGame::parseUpdateContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.get<char>();
	addGameTask(&Game::playerUpdateContainer, player->getID(), cid);
}

void ProtocolGame::parseThrow(NetworkMessage& msg)
{
	Position fromPos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	int16_t fromStackpos = msg.get<char>();
	Position toPos = msg.getPosition();
	uint8_t count = msg.get<char>();
	if (toPos != fromPos)
		addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerMoveThing,
			player->getID(), fromPos, spriteId, fromStackpos, toPos, count);
}

void ProtocolGame::parseLookAt(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	int16_t stackpos = msg.get<char>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookAt, player->getID(), pos, spriteId, stackpos);
}

void ProtocolGame::parseLookInBattleList(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInBattleList, player->getID(), creatureId);
}

void ProtocolGame::parseSay(NetworkMessage& msg)
{
	std::string receiver;
	uint16_t channelId = 0;

	MessageClasses type = (MessageClasses)msg.get<char>();
	switch (type)
	{
	case MSG_PRIVATE:
	case MSG_GAMEMASTER_PRIVATE:
	case MSG_RVR_ANSWER:
		receiver = msg.getString();
		break;

	case MSG_CHANNEL:
	case MSG_CHANNEL_HIGHLIGHT:
	case MSG_GAMEMASTER_CHANNEL:
	case MSG_GAMEMASTER_ANONYMOUS:
		channelId = msg.get<uint16_t>();
		break;

	default:
		break;
	}

	if (m_spectator)
	{
		Dispatcher::getInstance().addTask(createTask(boost::bind(&Spectators::handle, player->client, this, msg.getString(), channelId)));
		return;
	}

	const std::string text = msg.getString();
	if (text.length() > 255) //client limit
	{
		std::ostringstream s;
		s << "Attempt to send message with size " << text.length() << " - client is limited to 255 characters.";
		Logger::getInstance()->eFile("bots/" + player->getName() + ".log", s.str(), true);
		return;
	}

	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerSay, player->getID(), channelId, type, receiver, text);
}

void ProtocolGame::parseFightModes(NetworkMessage& msg)
{
	uint8_t rawFightMode = msg.get<char>(); //1 - offensive, 2 - balanced, 3 - defensive
	uint8_t rawChaseMode = msg.get<char>(); //0 - stand while fightning, 1 - chase opponent
	uint8_t rawSecureMode = msg.get<char>(); //0 - can't attack unmarked, 1 - can attack unmarked

	chaseMode_t chaseMode = CHASEMODE_STANDSTILL;
	if (rawChaseMode == 1)
		chaseMode = CHASEMODE_FOLLOW;

	fightMode_t fightMode = FIGHTMODE_ATTACK;
	if (rawFightMode == 2)
		fightMode = FIGHTMODE_BALANCED;
	else if (rawFightMode == 3)
		fightMode = FIGHTMODE_DEFENSE;

	secureMode_t secureMode = SECUREMODE_OFF;
	if (rawSecureMode == 1)
		secureMode = SECUREMODE_ON;

	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerSetFightModes, player->getID(), fightMode, chaseMode, secureMode);
}

void ProtocolGame::parseAttack(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	// msg.get<uint32_t>(); creatureId (same as above)
	addGameTask(&Game::playerSetAttackedCreature, player->getID(), creatureId);
}

void ProtocolGame::parseFollow(NetworkMessage& msg)
{
	uint32_t creatureId = msg.get<uint32_t>();
	// msg.get<uint32_t>(); creatureId (same as above)
	addGameTask(&Game::playerFollowCreature, player->getID(), creatureId);
}

void ProtocolGame::parseTextWindow(NetworkMessage& msg)
{
	uint32_t windowTextId = msg.get<uint32_t>();
	const std::string newText = msg.getString();
	addGameTask(&Game::playerWriteItem, player->getID(), windowTextId, newText);
}

void ProtocolGame::parseHouseWindow(NetworkMessage& msg)
{
	uint8_t doorId = msg.get<char>();
	uint32_t id = msg.get<uint32_t>();
	const std::string text = msg.getString();
	addGameTask(&Game::playerUpdateHouseWindow, player->getID(), doorId, id, text);
}

void ProtocolGame::parseRequestTrade(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	int16_t stackpos = msg.get<char>();
	uint32_t playerId = msg.get<uint32_t>();
	addGameTask(&Game::playerRequestTrade, player->getID(), pos, stackpos, playerId, spriteId);
}

void ProtocolGame::parseAcceptTrade(NetworkMessage&)
{
	addGameTask(&Game::playerAcceptTrade, player->getID());
}

void ProtocolGame::parseLookInTrade(NetworkMessage& msg)
{
	bool counter = (msg.get<char>() != (char)0);
	int32_t index = msg.get<char>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInTrade, player->getID(), counter, index);
}

void ProtocolGame::parseCloseTrade()
{
	addGameTask(&Game::playerCloseTrade, player->getID());
}

void ProtocolGame::parseAddVip(NetworkMessage& msg)
{
	const std::string name = msg.getString();
	if (name.size() > 32)
		return;

	addGameTask(&Game::playerRequestAddVip, player->getID(), name);
}

void ProtocolGame::parseRemoveVip(NetworkMessage& msg)
{
	uint32_t guid = msg.get<uint32_t>();
	addGameTask(&Game::playerRequestRemoveVip, player->getID(), guid);
}

void ProtocolGame::parseRotateItem(NetworkMessage& msg)
{
	Position pos = msg.getPosition();
	uint16_t spriteId = msg.get<uint16_t>();
	int16_t stackpos = msg.get<char>();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerRotateItem, player->getID(), pos, stackpos, spriteId);
}

void ProtocolGame::parseDebugAssert(NetworkMessage& msg)
{
	if (m_debugAssertSent)
		return;

	std::ostringstream s;
	s << "----- " << formatDate() << " - " << player->getName() << " (" << convertIPAddress(getIP())
		<< ") -----" << std::endl
		<< msg.getString() << std::endl
		<< msg.getString() << std::endl
		<< msg.getString() << std::endl
		<< msg.getString() << std::endl
		<< std::endl;

	m_debugAssertSent = true;
	Logger::getInstance()->iFile(LOGFILE_ASSERTIONS, s.str(), false);
}

void ProtocolGame::parseBugReport(NetworkMessage& msg)
{
	std::string comment = msg.getString();
	addGameTask(&Game::playerReportBug, player->getID(), comment);
}

void ProtocolGame::parseInviteToParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerInviteToParty, player->getID(), targetId);
}

void ProtocolGame::parseJoinParty(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerJoinParty, player->getID(), targetId);
}

void ProtocolGame::parseRevokePartyInvite(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerRevokePartyInvitation, player->getID(), targetId);
}

void ProtocolGame::parsePassPartyLeadership(NetworkMessage& msg)
{
	uint32_t targetId = msg.get<uint32_t>();
	addGameTask(&Game::playerPassPartyLeadership, player->getID(), targetId);
}

void ProtocolGame::parseLeaveParty(NetworkMessage&)
{
	addGameTask(&Game::playerLeaveParty, player->getID(), false);
}

//********************** Send methods *******************************//
void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xAD);
	msg->addString(receiver);
}

void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x8E);
	msg->add<uint32_t>(creature->getID());
	AddCreatureOutfit(msg, creature, outfit);
}

void ProtocolGame::sendCreatureLight(const Creature* creature)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureLight(msg, creature);
}

void ProtocolGame::sendWorldLight(const LightInfo& lightInfo)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddWorldLight(msg, lightInfo);
}

void ProtocolGame::sendCreatureShield(const Creature* creature)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x91);
	msg->add<uint32_t>(creature->getID());
	msg->addByte(player->getPartyShield(creature));
}

void ProtocolGame::sendCreatureSkull(const Creature* creature)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x90);
	msg->add<uint32_t>(creature->getID());
	msg->addByte(player->getSkullType(creature));
}

void ProtocolGame::sendCreatureSquare(const Creature* creature, uint8_t color)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x86);
	msg->add<uint32_t>(creature->getID());
	msg->addByte(color);
}

void ProtocolGame::sendStats()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddPlayerStats(msg);
}

void ProtocolGame::sendTextMessage(MessageClasses mClass, const std::string& message)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddTextMessage(msg, mClass, message);
}

void ProtocolGame::sendClosePrivate(uint16_t channelId)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	if (channelId == CHANNEL_GUILD || channelId == CHANNEL_PARTY)
		g_chat.removeUserFromChannel(player, channelId);

	msg->addByte(0xB3);
	msg->add<uint16_t>(channelId);
}

void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xB2);
	msg->add<uint16_t>(channelId);
	msg->addString(channelName);
}

void ProtocolGame::sendChannelsDialog(const ChannelsList& channels)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xAB);

	msg->addByte(channels.size());
	for (ChannelsList::const_iterator it = channels.begin(); it != channels.end(); ++it)
	{
		msg->add<uint16_t>(it->first);
		msg->addString(it->second);
	}
}

void ProtocolGame::sendChannel(uint16_t channelId, const std::string& channelName)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xAC);

	msg->add<uint16_t>(channelId);
	msg->addString(channelName);
}

void ProtocolGame::sendIcons(int32_t icons)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xA2);
	msg->addByte(icons);
}

void ProtocolGame::sendContainer(uint32_t cid, const Container* container, bool hasParent)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x6E);
	msg->addByte(cid);

	msg->addItem(container);
	msg->addString(container->getName());
	msg->addByte(container->capacity());

	msg->addByte(hasParent ? 0x01 : 0x00);
	msg->addByte(std::min(container->size(), 255U));

	ItemList::const_iterator cit = container->getItems();
	for (uint32_t i = 0; cit != container->getEnd() && i < 255; ++cit, ++i)
		msg->addItem(*cit);
}

void ProtocolGame::sendRuleViolationsChannel(uint16_t channelId)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (msg)
	{
		TRACK_MESSAGE(msg);
		msg->addByte(0xAE);
		msg->add<uint16_t>(channelId);
		for (RuleViolationsMap::const_iterator it = g_game.getRuleViolations().begin(); it != g_game.getRuleViolations().end(); ++it)
		{
			RuleViolation& rvr = *it->second;
			if (rvr.isOpen && rvr.reporter)
				AddCreatureSpeak(msg, rvr.reporter, MSG_RVR_CHANNEL, rvr.text, channelId, NULL, rvr.time);
		}
	}
}

void ProtocolGame::sendRemoveReport(const std::string& name)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (msg)
	{
		TRACK_MESSAGE(msg);
		msg->addByte(0xAF);
		msg->addString(name);
	}
}

void ProtocolGame::sendRuleViolationCancel(const std::string& name)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (msg)
	{
		TRACK_MESSAGE(msg);
		msg->addByte(0xB0);
		msg->addString(name);
	}
}

void ProtocolGame::sendLockRuleViolation()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (msg)
	{
		TRACK_MESSAGE(msg);
		msg->addByte(0xB1);
	}
}

void ProtocolGame::sendTradeItemRequest(const Player* _player, const Item* item, bool ack)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	if (ack)
		msg->addByte(0x7D);
	else
		msg->addByte(0x7E);

	msg->addString(_player->getName());
	if (const Container * container = item->getContainer())
	{
		msg->addByte(std::min(255U, container->getItemHoldingCount() + 1));
		msg->addItem(item);

		uint16_t i = 0;
		for (ContainerIterator it = container->begin(); i < 255 && it != container->end(); ++it, ++i)
			msg->addItem(*it);
	}
	else
	{
		msg->addByte(1);
		msg->addItem(item);
	}
}

void ProtocolGame::sendCloseTrade()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x7F);
}

void ProtocolGame::sendCloseContainer(uint32_t cid)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x6F);
	msg->addByte(cid);
}

void ProtocolGame::sendCreatureTurn(const Creature* creature, int16_t stackpos)
{
	if (stackpos >= 10 || !canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x6B);
	msg->addPosition(creature->getPosition());
	msg->addByte(stackpos);
	msg->add<uint16_t>(0x63);
	msg->add<uint32_t>(creature->getID());
	msg->addByte(creature->getDirection());
}

void ProtocolGame::sendCreatureSay(const Creature* creature, MessageClasses type, const std::string& text, Position* pos, uint32_t statementId)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureSpeak(msg, creature, type, text, 0, pos, statementId);
}

void ProtocolGame::sendCreatureChannelSay(const Creature* creature, MessageClasses type, const std::string& text, uint16_t channelId, uint32_t statementId)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureSpeak(msg, creature, type, text, channelId, NULL, statementId);
}

void ProtocolGame::sendCancel(const std::string& message)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddTextMessage(msg, MSG_STATUS_SMALL, message);
}

void ProtocolGame::sendCancelTarget()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xA3);
}

void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x8F);
	msg->add<uint32_t>(creature->getID());
	msg->add<uint16_t>(speed);
}

void ProtocolGame::sendCancelWalk()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xB5);
	msg->addByte(player->getDirection());
}

void ProtocolGame::sendSkills()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddPlayerSkills(msg);
}

void ProtocolGame::sendPing()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x1E);
}

void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type)
{
	if (type > SHOOT_EFFECT_LAST || (!canSee(from) && !canSee(to)))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddDistanceShoot(msg, from, to, type);
}

void ProtocolGame::sendMagicEffect(const Position& pos, uint8_t type)
{
	if (type > MAGIC_EFFECT_LAST || !canSee(pos))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddMagicEffect(msg, pos, type);
}

void ProtocolGame::sendAnimatedText(const Position& pos, uint8_t color, std::string text)
{
	if (!canSee(pos))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddAnimatedText(msg, pos, color, text);
}

void ProtocolGame::sendCreatureHealth(const Creature* creature)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddCreatureHealth(msg, creature);
}

//tile
void ProtocolGame::sendAddTileItem(const Tile*, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddTileItem(msg, pos, stackpos, item);
}

void ProtocolGame::sendUpdateTileItem(const Tile*, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (!canSee(pos))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	UpdateTileItem(msg, pos, stackpos, item);
}

void ProtocolGame::sendRemoveTileItem(const Tile*, const Position& pos, uint32_t stackpos)
{
	if (!canSee(pos))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveTileItem(msg, pos, stackpos);
}

void ProtocolGame::sendUpdateTile(const Tile* tile, const Position& pos)
{
	if (!canSee(pos))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x69);
	msg->addPosition(pos);
	if (tile)
	{
		GetTileDescription(tile, msg);
		msg->addByte(0x00);
		msg->addByte(0xFF);
	}
	else
	{
		msg->addByte(0x01);
		msg->addByte(0xFF);
	}
}

void ProtocolGame::sendAddCreature(const Creature* creature, const Position& pos, uint32_t stackpos)
{
	if (!canSee(creature))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	if (creature != player)
	{
		AddTileCreature(msg, pos, stackpos, creature);
		return;
	}

	msg->addByte(0x0A);
	msg->add<uint32_t>(player->getID());
	msg->addByte(0x32);
	msg->addByte(0x00);

	msg->addByte(player->hasFlag(PlayerFlag_CanReportBugs));
	if (Group * group = player->getGroup())
	{
		int32_t reasons = group->getViolationReasons();
		if (reasons > 1)
		{
			msg->addByte(0x0B);
			for (int32_t i = 0; i < 32; ++i)
			{
				msg->addByte(0xFF);
			}
		}
	}

	AddMapDescription(msg, pos);
	for (int32_t i = SLOT_FIRST; i < SLOT_LAST; ++i)
		AddInventoryItem(msg, (slots_t)i, player->getInventoryItem((slots_t)i));

	AddPlayerStats(msg);
	AddPlayerSkills(msg);

	LightInfo lightInfo;
	g_game.getWorldLightInfo(lightInfo);

	AddWorldLight(msg, lightInfo);
	AddCreatureLight(msg, creature);

	player->sendIcons();
	if (m_spectator)
		return;

	for (VIPSet::iterator it = player->VIPList.begin(); it != player->VIPList.end(); ++it)
	{
		std::string vipName;
		if (IOLoginData::getInstance()->getNameByGuid((*it), vipName))
		{
			Player* tmpPlayer = g_game.getPlayerByName(vipName);
			sendVIP((*it), vipName, (tmpPlayer && player->canSeeCreature(tmpPlayer)));
		}
	}
}

void ProtocolGame::sendRemoveCreature(const Creature*, const Position& pos, uint32_t stackpos)
{
	if (!canSee(pos))
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveTileItem(msg, pos, stackpos);
}

void ProtocolGame::sendMoveCreature(const Creature* creature, const Tile*, const Position& newPos,
	uint32_t newStackpos, const Tile*, const Position& oldPos, uint32_t oldStackpos, bool teleport)
{
	if (creature == player)
	{
		OutputMessage_ptr msg = getOutputBuffer();
		if (!msg)
			return;

		TRACK_MESSAGE(msg);
		if (teleport || oldStackpos >= 10)
		{
			RemoveTileItem(msg, oldPos, oldStackpos);
			AddMapDescription(msg, newPos);
		}
		else
		{
			if (oldPos.z != 7 || newPos.z < 8)
			{
				msg->addByte(0x6D);
				msg->addPosition(oldPos);
				msg->addByte(oldStackpos);
				msg->addPosition(newPos);
			}
			else
				RemoveTileItem(msg, oldPos, oldStackpos);

			if (newPos.z > oldPos.z)
				MoveDownCreature(msg, creature, newPos, oldPos, oldStackpos);
			else if (newPos.z < oldPos.z)
				MoveUpCreature(msg, creature, newPos, oldPos, oldStackpos);

			if (oldPos.y > newPos.y) // north, for old x
			{
				msg->addByte(0x65);
				GetMapDescription(oldPos.x - 8, newPos.y - 6, newPos.z, 18, 1, msg);
			}
			else if (oldPos.y < newPos.y) // south, for old x
			{
				msg->addByte(0x67);
				GetMapDescription(oldPos.x - 8, newPos.y + 7, newPos.z, 18, 1, msg);
			}

			if (oldPos.x < newPos.x) // east, [with new y]
			{
				msg->addByte(0x66);
				GetMapDescription(newPos.x + 9, newPos.y - 6, newPos.z, 1, 14, msg);
			}
			else if (oldPos.x > newPos.x) // west, [with new y]
			{
				msg->addByte(0x68);
				GetMapDescription(newPos.x - 8, newPos.y - 6, newPos.z, 1, 14, msg);
			}
		}
	}
	else if (canSee(oldPos) && canSee(newPos))
	{
		if (!player->canSeeCreature(creature))
			return;

		OutputMessage_ptr msg = getOutputBuffer();
		if (!msg)
			return;

		TRACK_MESSAGE(msg);
		if (!teleport && (oldPos.z != 7 || newPos.z < 8) && oldStackpos < 10)
		{
			msg->addByte(0x6D);
			msg->addPosition(oldPos);
			msg->addByte(oldStackpos);
			msg->addPosition(newPos);
		}
		else
		{
			RemoveTileItem(msg, oldPos, oldStackpos);
			AddTileCreature(msg, newPos, newStackpos, creature);
		}
	}
	else if (canSee(oldPos))
	{
		if (!player->canSeeCreature(creature))
			return;

		OutputMessage_ptr msg = getOutputBuffer();
		if (!msg)
			return;

		TRACK_MESSAGE(msg);
		RemoveTileItem(msg, oldPos, oldStackpos);
	}
	else if (canSee(newPos) && player->canSeeCreature(creature))
	{
		OutputMessage_ptr msg = getOutputBuffer();
		if (!msg)
			return;

		TRACK_MESSAGE(msg);
		AddTileCreature(msg, newPos, newStackpos, creature);
	}
}

//inventory
void ProtocolGame::sendAddInventoryItem(slots_t slot, const Item* item)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddInventoryItem(msg, slot, item);
}

void ProtocolGame::sendUpdateInventoryItem(slots_t slot, const Item* item)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	UpdateInventoryItem(msg, slot, item);
}

void ProtocolGame::sendRemoveInventoryItem(slots_t slot)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveInventoryItem(msg, slot);
}

//containers
void ProtocolGame::sendAddContainerItem(uint8_t cid, const Item* item)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	AddContainerItem(msg, cid, item);
}

void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint8_t slot, const Item* item)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	UpdateContainerItem(msg, cid, slot, item);
}

void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint8_t slot)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	RemoveContainerItem(msg, cid, slot);
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxLen, bool canWrite)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x96);
	msg->add<uint32_t>(windowTextId);
	msg->addItem(item);// check this
	if (canWrite)
	{
		msg->add<uint16_t>(maxLen);
		msg->addString(item->getText());
	}
	else
	{
		msg->add<uint16_t>(item->getText().size());
		msg->addString(item->getText());
	}

#ifdef _MULTIPLATFORM76
	const std::string& writer = item->getWriter();
	if (writer.size())
		msg->addString(writer);
	else
		msg->addString("");
#endif
}

void ProtocolGame::sendHouseWindow(uint32_t windowTextId, House*,
	uint32_t, const std::string& text)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x97);
	msg->addByte(0x00);
	msg->add<uint32_t>(windowTextId);
	msg->addString(text);
}

void ProtocolGame::sendOutfitWindow()
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xC8);
	AddCreatureOutfit(msg, player, player->getDefaultOutfit(), true);

#ifdef _MULTIPLATFORM77
	msg->add<uint16_t>(player->sex % 2 ? 362 : 410);
	msg->add<uint16_t>(player->isPremium() ? (player->sex % 2 ? 408 : 456) : (player->sex % 2 ? 371 : 419));
#else
	msg->addByte(player->sex % 2 ? 362 : 410);
	msg->addByte(player->isPremium() ? (player->sex % 2 ? 408 : 456) : (player->sex % 2 ? 371 : 419));
#endif

	player->hasRequestedOutfit(true);
}

void ProtocolGame::sendVIPLogIn(uint32_t guid)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xD3);
	msg->add<uint32_t>(guid);
}

void ProtocolGame::sendVIPLogOut(uint32_t guid)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xD4);
	msg->add<uint32_t>(guid);
}

void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, bool online)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xD2);
	msg->add<uint32_t>(guid);
	msg->addString(name);
	msg->addByte(online ? 1 : 0);
}

void ProtocolGame::reloadCreature(const Creature* creature)
{
	if (!canSee(creature))
		return;

	// we are cheating the client in here!
	uint32_t stackpos = creature->getTile()->getClientIndexOfThing(player, creature);
	if (stackpos >= 10)
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	if (std::find(knownCreatureList.begin(), knownCreatureList.end(),
		creature->getID()) != knownCreatureList.end())
	{
		RemoveTileItem(msg, creature->getPosition(), stackpos);
		msg->addByte(0x6A);

		msg->addPosition(creature->getPosition());
		msg->addByte(stackpos);
		AddCreature(msg, creature, false, creature->getID());
	}
	else
		AddTileCreature(msg, creature->getPosition(), stackpos, creature);
}

void ProtocolGame::AddMapDescription(OutputMessage_ptr msg, const Position& pos)
{
	msg->addByte(0x64);
	msg->addPosition(player->getPosition());
	GetMapDescription(pos.x - 8, pos.y - 6, pos.z, 18, 14, msg);
}

void ProtocolGame::AddTextMessage(OutputMessage_ptr msg, MessageClasses mClass, const std::string& message)
{
	msg->addByte(0xB4);
	msg->addByte(mClass);
	msg->addString(message);
}

void ProtocolGame::AddAnimatedText(OutputMessage_ptr msg, const Position& pos,
	uint8_t color, const std::string& text)
{
	msg->addByte(0x84);
	msg->addPosition(pos);
	msg->addByte(color);
	msg->addString(text);
}

void ProtocolGame::AddMagicEffect(OutputMessage_ptr msg, const Position& pos, uint8_t type)
{
	msg->addByte(0x83);
	msg->addPosition(pos);
#ifdef _MULTIPLATFORM76
	msg->addByte(type + 1);
#else
	msg->addByte(type);
#endif
}

void ProtocolGame::AddDistanceShoot(OutputMessage_ptr msg, const Position& from, const Position& to,
	uint8_t type)
{
	msg->addByte(0x85);
	msg->addPosition(from);
	msg->addPosition(to);
#ifdef _MULTIPLATFORM76
	msg->addByte(type + 1);
#else
	msg->addByte(type);
#endif
}

void ProtocolGame::AddCreature(OutputMessage_ptr msg, const Creature* creature, bool known, uint32_t remove)
{
	if (!known)
	{
		msg->add<uint16_t>(0x61);
		msg->add<uint32_t>(remove);
		msg->add<uint32_t>(creature->getID());
		msg->addString(creature->getHideName() ? "" : creature->getName());
	}
	else
	{
		msg->add<uint16_t>(0x62);
		msg->add<uint32_t>(creature->getID());
	}

	if (!creature->getHideHealth())
		msg->addByte((uint8_t)std::ceil(creature->getHealth() * 100. / std::max(creature->getMaxHealth(), 1)));
	else
		msg->addByte(0x00);

	msg->addByte((uint8_t)creature->getDirection());
	AddCreatureOutfit(msg, creature, creature->getCurrentOutfit());

	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);

	msg->addByte(lightInfo.level);
	msg->addByte(lightInfo.color);

	msg->add<uint16_t>(creature->getStepSpeed());
	msg->addByte(player->getSkullType(creature));
	msg->addByte(player->getPartyShield(creature));
}

void ProtocolGame::AddPlayerStats(OutputMessage_ptr msg)
{
	msg->addByte(0xA0);
	msg->add<uint16_t>(player->getHealth());
	msg->add<uint16_t>(player->getPlayerInfo(PLAYERINFO_MAXHEALTH));
	msg->add<uint16_t>(int32_t(player->getFreeCapacity()));
	if (player->getExperience() >= 0x7FFFFFFF) // client debugs after 2,147,483,647 exp
		msg->add<uint32_t>(0);
	else
		msg->add<uint32_t>(player->getExperience());
#ifdef _MULTIPLATFORM76
	msg->add<uint16_t>(player->getPlayerInfo(PLAYERINFO_LEVEL));
#else
	msg->addByte(player->getPlayerInfo(PLAYERINFO_LEVEL));
#endif
	msg->addByte(player->getPlayerInfo(PLAYERINFO_LEVELPERCENT));
	msg->add<uint16_t>(player->getPlayerInfo(PLAYERINFO_MANA));
	msg->add<uint16_t>(player->getPlayerInfo(PLAYERINFO_MAXMANA));
	msg->addByte(player->getPlayerInfo(PLAYERINFO_MAGICLEVEL));
	msg->addByte(player->getPlayerInfo(PLAYERINFO_MAGICLEVELPERCENT));
#ifdef _MULTIPLATFORM76
	msg->addByte(player->getPlayerInfo(PLAYERINFO_SOUL));
#endif
}

void ProtocolGame::AddPlayerSkills(OutputMessage_ptr msg)
{
	msg->addByte(0xA1);
	for (uint8_t i = 0; i <= SKILL_LAST; ++i)
	{
		msg->addByte(player->getSkill((skills_t)i, SKILL_LEVEL));
		msg->addByte(player->getSkill((skills_t)i, SKILL_PERCENT));
	}
}

void ProtocolGame::AddCreatureSpeak(OutputMessage_ptr msg, const Creature* creature, MessageClasses type,
	std::string text, uint16_t channelId, Position* pos, uint32_t statementId)
{
	msg->addByte(0xAA);
#ifdef _MULTIPLATFORM77
	msg->add<uint32_t>(0);
#endif
	if (creature)
	{
		if (creature->getSpeakType() != MSG_NONE)
			type = creature->getSpeakType();

		switch (type)
		{
		case MSG_GAMEMASTER_ANONYMOUS:
			msg->addString("");
			break;
		case MSG_RVR_ANSWER:
			msg->addString("Gamemaster");
			break;
		default:
			msg->addString(!creature->getHideName() ? creature->getName() : "");
			break;
		}
	}
	else
		msg->addString("");

	msg->addByte(type);
	switch (type)
	{
	case MSG_SPEAK_SAY:
	case MSG_SPEAK_WHISPER:
	case MSG_SPEAK_YELL:
	case MSG_SPEAK_MONSTER_SAY:
	case MSG_SPEAK_MONSTER_YELL:
	{
		if (pos)
			msg->addPosition(*pos);
		else if (creature)
			msg->addPosition(creature->getPosition());
		else
			msg->addPosition(Position(0, 0, 7));

		break;
	}

	case MSG_CHANNEL:
	case MSG_CHANNEL_HIGHLIGHT:
	case MSG_GAMEMASTER_CHANNEL:
	case MSG_GAMEMASTER_ANONYMOUS:
		msg->add<uint16_t>(channelId);
		break;

	case MSG_RVR_CHANNEL:
	{
		msg->add<uint32_t>(uint32_t(OTSYS_TIME() / 1000 & 0xFFFFFFFF) - statementId/*use it as time:)*/);
		break;
	}

	default:
		break;
	}

	msg->addString(text);
}

void ProtocolGame::AddCreatureHealth(OutputMessage_ptr msg, const Creature* creature)
{
	msg->addByte(0x8C);
	msg->add<uint32_t>(creature->getID());
	if (!creature->getHideHealth())
		msg->addByte((uint8_t)std::ceil(creature->getHealth() * 100. / std::max(creature->getMaxHealth(), (int32_t)1)));
	else
		msg->addByte(0x00);
}

void ProtocolGame::AddCreatureOutfit(OutputMessage_ptr msg, const Creature* creature, const Outfit_t& outfit, bool outfitWindow/* = false*/)
{
	if (outfitWindow || (!creature->isInvisible() && (!creature->isGhost()
		|| !g_config.getBool(ConfigManager::GHOST_INVISIBLE_EFFECT))))
	{
#ifdef _MULTIPLATFORM77
		msg->add<uint16_t>(outfit.lookType);
#else
		msg->addByte(outfit.lookType);
#endif
		if (outfit.lookType)
		{
			msg->addByte(outfit.lookHead);
			msg->addByte(outfit.lookBody);
			msg->addByte(outfit.lookLegs);
			msg->addByte(outfit.lookFeet);
		}
		else if (outfit.lookTypeEx)
			msg->addItemId(outfit.lookTypeEx);
		else
			msg->add<uint16_t>(outfit.lookTypeEx);
	}
	else
	{
#ifdef _MULTIPLATFORM77
		msg->add<uint16_t>(0x00);
#else
		msg->addByte(0x00);
#endif
		msg->add<uint16_t>(0x00);
	}
}

void ProtocolGame::AddWorldLight(OutputMessage_ptr msg, const LightInfo& lightInfo)
{
	msg->addByte(0x82);
	msg->addByte(lightInfo.level);
	msg->addByte(lightInfo.color);
}

void ProtocolGame::AddCreatureLight(OutputMessage_ptr msg, const Creature* creature)
{
	msg->addByte(0x8D);
	msg->add<uint32_t>(creature->getID());

	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);

	msg->addByte(lightInfo.level);
	msg->addByte(lightInfo.color);
}

//tile
void ProtocolGame::AddTileItem(OutputMessage_ptr msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (stackpos >= 10)
		return;

	msg->addByte(0x6A);
	msg->addPosition(pos);
	msg->addItem(item);
}

void ProtocolGame::AddTileCreature(OutputMessage_ptr msg, const Position& pos, uint32_t stackpos, const Creature* creature)
{
	if (stackpos >= 10)
		return;

	msg->addByte(0x6A);
	msg->addPosition(pos);

	bool known;
	uint32_t removedKnown;
	checkCreatureAsKnown(creature->getID(), known, removedKnown);
	AddCreature(msg, creature, known, removedKnown);
}

void ProtocolGame::UpdateTileItem(OutputMessage_ptr msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if (stackpos >= 10)
		return;

	msg->addByte(0x6B);
	msg->addPosition(pos);
	msg->addByte(stackpos);
	msg->addItem(item);
}

void ProtocolGame::RemoveTileItem(OutputMessage_ptr msg, const Position& pos, uint32_t stackpos)
{
	if (stackpos >= 10)
		return;

	msg->addByte(0x6C);
	msg->addPosition(pos);
	msg->addByte(stackpos);
}

void ProtocolGame::MoveUpCreature(OutputMessage_ptr msg, const Creature* creature,
	const Position& newPos, const Position& oldPos, uint32_t)
{
	if (creature != player)
		return;

	msg->addByte(0xBE); //floor change up
	if (newPos.z == 7) //going to surface
	{
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 5, 18, 14, 3, skip); //(floor 7 and 6 already set)
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 4, 18, 14, 4, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 3, 18, 14, 5, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 2, 18, 14, 6, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 1, 18, 14, 7, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, 0, 18, 14, 8, skip);
		if (skip >= 0)
		{
			msg->addByte(skip);
			msg->addByte(0xFF);
		}
	}
	else if (newPos.z > 7) //underground, going one floor up (still underground)
	{
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, oldPos.z - 3, 18, 14, 3, skip);
		if (skip >= 0)
		{
			msg->addByte(skip);
			msg->addByte(0xFF);
		}
	}

	//moving up a floor up makes us out of sync
	//west
	msg->addByte(0x68);
	GetMapDescription(oldPos.x - 8, oldPos.y + 1 - 6, newPos.z, 1, 14, msg);

	//north
	msg->addByte(0x65);
	GetMapDescription(oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 1, msg);
}

void ProtocolGame::MoveDownCreature(OutputMessage_ptr msg, const Creature* creature,
	const Position& newPos, const Position& oldPos, uint32_t)
{
	if (creature != player)
		return;

	msg->addByte(0xBF); //floor change down
	if (newPos.z == 8) //going from surface to underground
	{
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z, 18, 14, -1, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 1, 18, 14, -2, skip);
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);
		if (skip >= 0)
		{
			msg->addByte(skip);
			msg->addByte(0xFF);
		}
	}
	else if (newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14) //going further down
	{
		int32_t skip = -1;
		GetFloorDescription(msg, oldPos.x - 8, oldPos.y - 6, newPos.z + 2, 18, 14, -3, skip);
		if (skip >= 0)
		{
			msg->addByte(skip);
			msg->addByte(0xFF);
		}
	}

	//moving down a floor makes us out of sync
	//east
	msg->addByte(0x66);
	GetMapDescription(oldPos.x + 9, oldPos.y - 1 - 6, newPos.z, 1, 14, msg);

	//south
	msg->addByte(0x67);
	GetMapDescription(oldPos.x - 8, oldPos.y + 7, newPos.z, 18, 1, msg);
}

//inventory
void ProtocolGame::AddInventoryItem(OutputMessage_ptr msg, slots_t slot, const Item* item)
{
	if (item)
	{
		msg->addByte(0x78);
		msg->addByte(slot);
		msg->addItem(item);
	}
	else
		RemoveInventoryItem(msg, slot);
}

void ProtocolGame::RemoveInventoryItem(OutputMessage_ptr msg, slots_t slot)
{
	msg->addByte(0x79);
	msg->addByte(slot);
}

void ProtocolGame::UpdateInventoryItem(OutputMessage_ptr msg, slots_t slot, const Item* item)
{
	AddInventoryItem(msg, slot, item);
}

//containers
void ProtocolGame::AddContainerItem(OutputMessage_ptr msg, uint8_t cid, const Item* item)
{
	msg->addByte(0x70);
	msg->addByte(cid);
	msg->addItem(item);
}

void ProtocolGame::UpdateContainerItem(OutputMessage_ptr msg, uint8_t cid, uint8_t slot, const Item* item)
{
	msg->addByte(0x71);
	msg->addByte(cid);
	msg->addByte(slot);
	msg->addItem(item);
}

void ProtocolGame::RemoveContainerItem(OutputMessage_ptr msg, uint8_t cid, uint8_t slot)
{
	msg->addByte(0x72);
	msg->addByte(cid);
	msg->addByte(slot);
}

void ProtocolGame::sendChannelMessage(std::string author, std::string text, MessageClasses type, uint16_t channel)
{
	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0xAA);
#ifdef _MULTIPLATFORM77
	msg->add<uint32_t>(0);
#endif
	msg->addString(author);
	msg->addByte(type);
	msg->add<uint16_t>(channel);
	msg->addString(text);
}

void ProtocolGame::parseExtendedOpcode(NetworkMessage& msg)
{
	uint8_t opcode = msg.get<char>();
	std::string buffer = msg.getString();

	// process additional opcodes via lua script event
	addGameTask(&Game::parsePlayerExtendedOpcode, player->getID(), opcode, buffer);
}

void ProtocolGame::sendExtendedOpcode(uint8_t opcode, const std::string& buffer)
{
	// extended opcodes can only be send to players using otclient, cipsoft's tibia can't understand them
	if (player && !player->isUsingOtclient())
		return;

	OutputMessage_ptr msg = getOutputBuffer();
	if (!msg)
		return;

	TRACK_MESSAGE(msg);
	msg->addByte(0x32);
	msg->addByte(opcode);
	msg->addString(buffer);
}
