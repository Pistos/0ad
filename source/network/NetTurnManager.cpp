/* Copyright (C) 2010 Wildfire Games.
 * This file is part of 0 A.D.
 *
 * 0 A.D. is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * 0 A.D. is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with 0 A.D.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "precompiled.h"

#include "NetTurnManager.h"

#include "network/NetServer.h"
#include "network/NetClient.h"
#include "network/NetMessage.h"

#include "gui/GUIManager.h"
#include "maths/MathUtil.h"
#include "ps/Profile.h"
#include "ps/Pyrogenesis.h"
#include "ps/Replay.h"
#include "scriptinterface/ScriptInterface.h"
#include "simulation2/Simulation2.h"

#include <sstream>
#include <fstream>
#include <iomanip>

static const int DEFAULT_TURN_LENGTH_MP = 500;
static const int DEFAULT_TURN_LENGTH_SP = 200;

static const int COMMAND_DELAY = 2;

#if 0
#define NETTURN_LOG(args) debug_printf args
#else
#define NETTURN_LOG(args)
#endif

static std::string Hexify(const std::string& s)
{
	std::stringstream str;
	str << std::hex;
	for (size_t i = 0; i < s.size(); ++i)
		str << std::setfill('0') << std::setw(2) << (int)(unsigned char)s[i];
	return str.str();
}

CNetTurnManager::CNetTurnManager(CSimulation2& simulation, u32 defaultTurnLength, int clientId, IReplayLogger& replay) :
	m_Simulation2(simulation), m_CurrentTurn(0), m_ReadyTurn(1), m_TurnLength(defaultTurnLength), m_DeltaTime(0),
	m_PlayerId(-1), m_ClientId(clientId), m_HasSyncError(false), m_Replay(replay)
{
	// When we are on turn n, we schedule new commands for n+2.
	// We know that all other clients have finished scheduling commands for n (else we couldn't have got here).
	// We know we have not yet finished scheduling commands for n+2.
	// Hence other clients can be on turn n-1, n, n+1, and no other.
	// So they can be sending us commands scheduled for n+1, n+2, n+3.
	// So we need a 3-element buffer:
	m_QueuedCommands.resize(COMMAND_DELAY + 1);
}

void CNetTurnManager::SetPlayerID(int playerId)
{
	m_PlayerId = playerId;
}

bool CNetTurnManager::Update(float frameLength, size_t maxTurns)
{
	m_DeltaTime += frameLength;

	// If we haven't reached the next turn yet, do nothing
	if (m_DeltaTime < 0)
		return false;

	NETTURN_LOG((L"Update current=%d ready=%d\n", m_CurrentTurn, m_ReadyTurn));

	// Check that the next turn is ready for execution
	if (m_ReadyTurn <= m_CurrentTurn)
	{
		// Oops, we wanted to start the next turn but it's not ready yet -
		// there must be too much network lag.
		// TODO: complain to the user.
		// TODO: send feedback to the server to increase the turn length.

		// Reset the next-turn timer to 0 so we try again next update but
		// so we don't rush to catch up in subsequent turns.
		// TODO: we should do clever rate adjustment instead of just pausing like this.
		m_DeltaTime = 0;

		return false;
	}

	maxTurns = std::max((size_t)1, maxTurns); // always do at least one turn

	for (size_t i = 0; i < maxTurns; ++i)
	{
		// Check that we've reached the i'th next turn
		if (m_DeltaTime < 0)
			break;

		// Check that the i'th next turn is still ready
		if (m_ReadyTurn <= m_CurrentTurn)
			break;

		NotifyFinishedOwnCommands(m_CurrentTurn + COMMAND_DELAY);

		m_CurrentTurn += 1; // increase the turn number now, so Update can send new commands for a subsequent turn

		// Save the current state for rewinding, if enabled
		if (m_TimeWarpNumTurns && (m_CurrentTurn % m_TimeWarpNumTurns) == 0)
		{
			PROFILE("time warp serialization");
			std::stringstream stream;
			m_Simulation2.SerializeState(stream);
			m_TimeWarpStates.push_back(stream.str());
		}

		// Put all the client commands into a single list, in a globally consistent order
		std::vector<SimulationCommand> commands;
		for (std::map<u32, std::vector<SimulationCommand> >::iterator it = m_QueuedCommands[0].begin(); it != m_QueuedCommands[0].end(); ++it)
		{
			commands.insert(commands.end(), it->second.begin(), it->second.end());
		}
		m_QueuedCommands.pop_front();
		m_QueuedCommands.resize(m_QueuedCommands.size() + 1);

		m_Replay.Turn(m_CurrentTurn-1, m_TurnLength, commands);

		NETTURN_LOG((L"Running %d cmds\n", commands.size()));

		m_Simulation2.Update(m_TurnLength, commands);

		NotifyFinishedUpdate(m_CurrentTurn);

		// Set the time for the next turn update
		m_DeltaTime -= m_TurnLength / 1000.f;
	}

	return true;
}

void CNetTurnManager::OnSyncError(u32 turn, const std::string& expectedHash)
{
	NETTURN_LOG((L"OnSyncError(%d, %hs)\n", turn, Hexify(expectedHash).c_str()));

	// Only complain the first time
	if (m_HasSyncError)
		return;
	m_HasSyncError = true;

	std::string hash;
	bool ok = m_Simulation2.ComputeStateHash(hash);
	debug_assert(ok);

	fs::wpath path (psLogDir()/L"oos_dump.txt");
	std::ofstream file (path.external_file_string().c_str(), std::ofstream::out | std::ofstream::trunc);
	m_Simulation2.DumpDebugState(file);
	file.close();

	std::wstringstream msg;
	msg << L"Out of sync on turn " << turn << L": expected hash " << CStrW(Hexify(expectedHash)) << L"\n\n";
	msg << L"Current state: turn " << m_CurrentTurn << L", hash " << CStrW(Hexify(hash)) << L"\n\n";
	msg << L"Dumping current state to " << path;
	g_GUI->DisplayMessageBox(600, 350, L"Sync error", msg.str());
}

void CNetTurnManager::Interpolate(float frameLength)
{
	// TODO: using m_TurnLength might be a bit dodgy when length changes - maybe
	// we need to save the previous turn length?

	float offset = clamp(m_DeltaTime / (m_TurnLength / 1000.f) + 1.0, 0.0, 1.0);
	m_Simulation2.Interpolate(frameLength, offset);
}

void CNetTurnManager::AddCommand(int client, int player, CScriptValRooted data, u32 turn)
{
	NETTURN_LOG((L"AddCommand(client=%d player=%d turn=%d)\n", client, player, turn));

	if (!(m_CurrentTurn < turn && turn <= m_CurrentTurn + COMMAND_DELAY + 1))
	{
		debug_warn(L"Received command for invalid turn");
		return;
	}

	SimulationCommand cmd;
	cmd.player = player;
	cmd.data = data;
	m_QueuedCommands[turn - (m_CurrentTurn+1)][client].push_back(cmd);
}

void CNetTurnManager::FinishedAllCommands(u32 turn, u32 turnLength)
{
	NETTURN_LOG((L"FinishedAllCommands(%d, %d)\n", turn, turnLength));

	debug_assert(turn == m_ReadyTurn + 1);
	m_ReadyTurn = turn;
	m_TurnLength = turnLength;
}

void CNetTurnManager::EnableTimeWarpRecording(size_t numTurns)
{
	m_TimeWarpStates.clear();
	m_TimeWarpNumTurns = numTurns;
}

void CNetTurnManager::RewindTimeWarp()
{
	if (m_TimeWarpStates.empty())
		return;

	std::stringstream stream(m_TimeWarpStates.back());
	m_Simulation2.DeserializeState(stream);
	m_TimeWarpStates.pop_back();

	// Reset the turn manager state, so we won't execute stray commands and
	// won't do the next snapshot until the appropriate time.
	// (Ideally we ought to serialise the turn manager state and restore it
	// here, but this is simpler for now.)
	m_CurrentTurn = 0;
	m_ReadyTurn = 1;
	m_DeltaTime = 0;
	size_t queuedCommandsSize = m_QueuedCommands.size();
	m_QueuedCommands.clear();
	m_QueuedCommands.resize(queuedCommandsSize);
}



CNetClientTurnManager::CNetClientTurnManager(CSimulation2& simulation, CNetClient& client, int clientId, IReplayLogger& replay) :
	CNetTurnManager(simulation, DEFAULT_TURN_LENGTH_MP, clientId, replay), m_NetClient(client)
{
}

void CNetClientTurnManager::PostCommand(CScriptValRooted data)
{
	NETTURN_LOG((L"PostCommand()\n"));

	// Transmit command to server
	CSimulationMessage msg(m_Simulation2.GetScriptInterface(), m_ClientId, m_PlayerId, m_CurrentTurn + COMMAND_DELAY, data.get());
	m_NetClient.SendMessage(&msg);

	// Add to our local queue
	//AddCommand(m_ClientId, m_PlayerId, data, m_CurrentTurn + COMMAND_DELAY);
	// TODO: we should do this when the server stops sending our commands back to us
}

void CNetClientTurnManager::NotifyFinishedOwnCommands(u32 turn)
{
	NETTURN_LOG((L"NotifyFinishedOwnCommands(%d)\n", turn));

	// Send message to the server
	CEndCommandBatchMessage msg;
	msg.m_TurnLength = DEFAULT_TURN_LENGTH_MP; // TODO: why do we send this?
	msg.m_Turn = turn;
	m_NetClient.SendMessage(&msg);
}

void CNetClientTurnManager::NotifyFinishedUpdate(u32 turn)
{
	std::string hash;
	{
		PROFILE("state hash check");
		bool ok = m_Simulation2.ComputeStateHash(hash);
		debug_assert(ok);
	}

	NETTURN_LOG((L"NotifyFinishedUpdate(%d, %hs)\n", turn, Hexify(hash).c_str()));

	m_Replay.Hash(hash);

	// Send message to the server
	CSyncCheckMessage msg;
	msg.m_Turn = turn;
	msg.m_Hash = hash;
	m_NetClient.SendMessage(&msg);
}

void CNetClientTurnManager::OnSimulationMessage(CSimulationMessage* msg)
{
	// Command received from the server - store it for later execution
	AddCommand(msg->m_Client, msg->m_Player, msg->m_Data, msg->m_Turn);
}


CNetLocalTurnManager::CNetLocalTurnManager(CSimulation2& simulation, IReplayLogger& replay) :
	CNetTurnManager(simulation, DEFAULT_TURN_LENGTH_SP, 0, replay)
{
}

void CNetLocalTurnManager::PostCommand(CScriptValRooted data)
{
	// Add directly to the next turn, ignoring COMMAND_DELAY,
	// because we don't need to compensate for network latency
	AddCommand(m_ClientId, m_PlayerId, data, m_CurrentTurn + 1);
}

void CNetLocalTurnManager::NotifyFinishedOwnCommands(u32 turn)
{
	FinishedAllCommands(turn, m_TurnLength);
}

void CNetLocalTurnManager::NotifyFinishedUpdate(u32 UNUSED(turn))
{
#if 0 // this hurts performance and is only useful for verifying log replays
	std::string hash;
	{
		PROFILE("state hash check");
		bool ok = m_Simulation2.ComputeStateHash(hash);
		debug_assert(ok);
	}
	m_Replay.Hash(hash);
#endif
}

void CNetLocalTurnManager::OnSimulationMessage(CSimulationMessage* UNUSED(msg))
{
	debug_warn(L"This should never be called");
}




CNetServerTurnManager::CNetServerTurnManager(CNetServerWorker& server) :
	m_NetServer(server), m_ReadyTurn(1), m_TurnLength(DEFAULT_TURN_LENGTH_MP)
{
}

void CNetServerTurnManager::NotifyFinishedClientCommands(int client, u32 turn)
{
	NETTURN_LOG((L"NotifyFinishedClientCommands(client=%d, turn=%d)\n", client, turn));

	// Must be a client we've already heard of
	debug_assert(m_ClientsReady.find(client) != m_ClientsReady.end());

	// Clients must advance one turn at a time
	debug_assert(turn == m_ClientsReady[client] + 1);
	m_ClientsReady[client] = turn;

	// Check whether this was the final client to become ready
	CheckClientsReady();
}

void CNetServerTurnManager::CheckClientsReady()
{
	// See if all clients (including self) are ready for a new turn
	for (std::map<int, u32>::iterator it = m_ClientsReady.begin(); it != m_ClientsReady.end(); ++it)
	{
		NETTURN_LOG((L"  %d: %d <=? %d\n", it->first, it->second, m_ReadyTurn));
		if (it->second <= m_ReadyTurn)
			return; // wasn't ready for m_ReadyTurn+1
	}

	// Advance the turn
	++m_ReadyTurn;

	NETTURN_LOG((L"CheckClientsReady: ready for turn %d\n", m_ReadyTurn));

	// Tell all clients that the next turn is ready
	CEndCommandBatchMessage msg;
	msg.m_TurnLength = m_TurnLength;
	msg.m_Turn = m_ReadyTurn;
	m_NetServer.Broadcast(&msg);
}

void CNetServerTurnManager::NotifyFinishedClientUpdate(int client, u32 turn, const std::string& hash)
{
	// Clients must advance one turn at a time
	debug_assert(turn == m_ClientsSimulated[client] + 1);
	m_ClientsSimulated[client] = turn;

	m_ClientStateHashes[turn][client] = hash;

	// Find the newest turn which we know all clients have simulated
	u32 newest = std::numeric_limits<u32>::max();
	for (std::map<int, u32>::iterator it = m_ClientsSimulated.begin(); it != m_ClientsSimulated.end(); ++it)
	{
		if (it->second < newest)
			newest = it->second;
	}

	// For every set of state hashes that all clients have simulated, check for OOS
	for (std::map<u32, std::map<int, std::string> >::iterator it = m_ClientStateHashes.begin(); it != m_ClientStateHashes.end(); ++it)
	{
		if (it->first > newest)
			break;

		// Assume the host is correct (maybe we should choose the most common instead to help debugging)
		std::string expected = it->second.begin()->second;

		for (std::map<int, std::string>::iterator cit = it->second.begin(); cit != it->second.end(); ++cit)
		{
			NETTURN_LOG((L"sync check %d: %d = %hs\n", it->first, cit->first, Hexify(cit->second).c_str()));
			if (cit->second != expected)
			{
				// Oh no, out of sync

				// Tell everyone about it
				CSyncErrorMessage msg;
				msg.m_Turn = it->first;
				msg.m_HashExpected = expected;
				m_NetServer.Broadcast(&msg);

				break;
			}
		}
	}

	// Delete the saved hashes for all turns that we've already verified
	m_ClientStateHashes.erase(m_ClientStateHashes.begin(), m_ClientStateHashes.lower_bound(newest+1));
}

void CNetServerTurnManager::InitialiseClient(int client)
{
	NETTURN_LOG((L"InitialiseClient(client=%d)\n", client));

	debug_assert(m_ClientsReady.find(client) == m_ClientsReady.end());
	m_ClientsReady[client] = 1;
	m_ClientsSimulated[client] = 0;
}

void CNetServerTurnManager::UninitialiseClient(int client)
{
	NETTURN_LOG((L"UninitialiseClient(client=%d)\n", client));

	debug_assert(m_ClientsReady.find(client) != m_ClientsReady.end());
	m_ClientsReady.erase(client);
	m_ClientsSimulated.erase(client);

	// Check whether we're ready for the next turn now that we're not
	// waiting for this client any more
	CheckClientsReady();
}

void CNetServerTurnManager::SetTurnLength(u32 msecs)
{
	m_TurnLength = msecs;
}
