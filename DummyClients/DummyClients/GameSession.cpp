#include "stdafx.h"
#include "Exception.h"
#include "FastSpinlock.h"
#include "DummyClients.h"
#include "PlayerSession.h"
#include "GameSession.h"
#include "IocpManager.h"
#include "GameLiftManager.h"
#include "PacketType.h"
#include "DummyClients.h"
#include "Log.h"

#include <Rpc.h>
#include <aws/core/utils/Outcome.h>
#include <aws/gamelift/model/SearchGameSessionsRequest.h>
#include <aws/gamelift/model/DescribeGameSessionsRequest.h>
#include <aws/gamelift/model/CreateGameSessionRequest.h>
#include <aws/gamelift/model/CreatePlayerSessionsRequest.h>
#include <aws/gamelift/model/StartGameSessionPlacementRequest.h>
#include <aws/gamelift/model/DescribeGameSessionPlacementRequest.h>
#include <aws/gamelift/model/DescribeGameSessionDetailsRequest.h>

GameSession::~GameSession()
{
	for (auto it : mReadySessionList)
	{
		delete it;
	}
}

bool GameSession::PreparePlayerSessions()
{
	CRASH_ASSERT(LThreadType == THREAD_MAIN);

	for (int i = 0; i < mMaxPlayerCount - TEST_PLAYER_SESSION_EXCEPT; ++i)
	{
		PlayerSession* client = new PlayerSession(this);

		if (false == client->PrepareSession())
			return false;
			
		mReadySessionList.push_back(client);
	}

	return true;
}

bool GameSession::CreateGameSession()
{
	FastSpinlockGuard guard(mLock);

	Aws::GameLift::Model::CreateGameSessionRequest req;
	
	auto aliasId = GGameLiftManager->GetAliasId();
	if (aliasId == "TEST_LOCAL")
	{
		req.SetFleetId(std::string("fleet-") + mGameSessionName);
	}
	else
	{
		req.SetAliasId(aliasId);
	}
	
	req.SetName(mGameSessionName);
	req.SetMaximumPlayerSessionCount(mMaxPlayerCount);
	auto outcome = GGameLiftManager->GetAwsClient()->CreateGameSession(req);
	if (outcome.IsSuccess())
	{
		auto gs = outcome.GetResult().GetGameSession();
		mPort = gs.GetPort();
		mIpAddress = gs.GetIpAddress();
		mGameSessionId = gs.GetGameSessionId();

		/// wait until ACTIVE
		while (true)
		{
			Aws::GameLift::Model::DescribeGameSessionsRequest descReq;
			descReq.SetGameSessionId(mGameSessionId);
						
			auto descRet = GGameLiftManager->GetAwsClient()->DescribeGameSessions(descReq);
			if (descRet.IsSuccess())
			{
				auto status = descRet.GetResult().GetGameSessions()[0].GetStatus();
				if (status == Aws::GameLift::Model::GameSessionStatus::ACTIVE)
				{
					return true;
				}
			}
			else
			{
				GConsoleLog->PrintOut(true, "%s\n", descRet.GetError().GetMessageA().c_str());
				return false;
			}

			Sleep(500);
		}
	}
	 
	GConsoleLog->PrintOut(true, "CreateGameSession: %s\n", outcome.GetError().GetMessageA().c_str());

	return false;
}

bool GameSession::FindAvailableGameSession()
{
	FastSpinlockGuard guard(mLock);
	auto aliasId = GGameLiftManager->GetAliasId();
	if (aliasId == "TEST_LOCAL")
	{
		return false; ///< GameLift Local mode does not support to find a game session
	}

	Aws::GameLift::Model::SearchGameSessionsRequest searchReq;
	searchReq.SetAliasId(aliasId);
	searchReq.SetFilterExpression("playerSessionCount=0 AND hasAvailablePlayerSessions=true");
	searchReq.SetLimit(1);
	auto searchResult = GGameLiftManager->GetAwsClient()->SearchGameSessions(searchReq);
	if (searchResult.IsSuccess())
	{
		auto availableGs = searchResult.GetResult().GetGameSessions();
		if (availableGs.size() > 0)
		{
			mPort = availableGs[0].GetPort();
			mIpAddress = availableGs[0].GetIpAddress();
			mGameSessionId = availableGs[0].GetGameSessionId();
			return true;
		}
	}
	else
	{
		GConsoleLog->PrintOut(true, "FindAvailableGameSession: %s\n", searchResult.GetError().GetMessageA().c_str());
	}

	return false;
}

bool GameSession::CreatePlayerSessions()
{
	FastSpinlockGuard guard(mLock);

	Aws::GameLift::Model::CreatePlayerSessionsRequest req;
	req.SetGameSessionId(mGameSessionId);
	std::vector<std::string> playerIds;

	for (int i = 0; i < mMaxPlayerCount - TEST_PLAYER_SESSION_EXCEPT; ++i) ///< must be less than 25
	{
		std::string pid = "DummyPlayer" + std::to_string(mStartPlayerId + i);
		playerIds.push_back(pid);
	}
	req.SetPlayerIds(playerIds);

	auto outcome = GGameLiftManager->GetAwsClient()->CreatePlayerSessions(req);
	if (outcome.IsSuccess())
	{
		mPlayerSessionList = outcome.GetResult().GetPlayerSessions();
		return true;
	}

	GConsoleLog->PrintOut(true, "CreatePlayerSessions: %s\n", outcome.GetError().GetMessageA().c_str());
	return false;
}

bool GameSession::ConnectPlayerSessions()
{
	FastSpinlockGuard guard(mLock);

	auto it = mReadySessionList.begin();
	int idx = mStartPlayerId;
	for (auto& playerSessionItem : mPlayerSessionList)
	{
		(*it)->AddRef();
		if (false == (*it)->ConnectRequest(playerSessionItem.GetPlayerSessionId(), idx++))
			return false;

		Sleep(10);

		++it;
	}
	
	return true;
}

void GameSession::DisconnectPlayerSessions()
{
	FastSpinlockGuard guard(mLock);

	for (auto session : mReadySessionList)
	{
		if (session->IsConnected())
			session->DisconnectRequest(DR_ACTIVE);
	}
}

bool GameSession::StartGameSessionPlacement()
{
	FastSpinlockGuard guard(mLock);

	/// region reset for a match queue...
	GGameLiftManager->SetUpAwsClient(GGameLiftManager->GetRegion());

	GeneratePlacementId();

	Aws::GameLift::Model::StartGameSessionPlacementRequest req;
	req.SetGameSessionQueueName(GGameLiftManager->GetMatchQueue());
	req.SetMaximumPlayerSessionCount(MAX_PLAYER_PER_GAME);
	req.SetPlacementId(mPlacementId);

	auto outcome = GGameLiftManager->GetAwsClient()->StartGameSessionPlacement(req);
	if (outcome.IsSuccess())
	{
		auto status = outcome.GetResult().GetGameSessionPlacement().GetStatus();

		if (status == Aws::GameLift::Model::GameSessionPlacementState::PENDING)
		{
			return CheckGameSessionPlacement();
		}

		if (status == Aws::GameLift::Model::GameSessionPlacementState::FULFILLED)
		{
			auto gs = outcome.GetResult().GetGameSessionPlacement();

			mGameSessionId = gs.GetGameSessionArn();
			mIpAddress = gs.GetIpAddress();
			mPort = gs.GetPort();

			/// change region...
			GGameLiftManager->SetUpAwsClient(gs.GetGameSessionRegion());

			return true;
		}
	}

	GConsoleLog->PrintOut(true, "%s\n", outcome.GetError().GetMessageA().c_str());

	return false;
}

bool GameSession::CheckGameSessionPlacement()
{
	while (true)
	{
		Aws::GameLift::Model::DescribeGameSessionPlacementRequest req;
		req.SetPlacementId(mPlacementId);
		auto outcome = GGameLiftManager->GetAwsClient()->DescribeGameSessionPlacement(req);
		if (outcome.IsSuccess())
		{
			auto gs = outcome.GetResult().GetGameSessionPlacement();

			if (gs.GetStatus() == Aws::GameLift::Model::GameSessionPlacementState::FULFILLED)
			{
				mGameSessionId = gs.GetGameSessionArn();
				mIpAddress = gs.GetIpAddress();
				mPort = gs.GetPort();

				/// change region...
				GGameLiftManager->SetUpAwsClient(gs.GetGameSessionRegion());
				
				return true;
			}
		}
		else
		{
			break;
		}

		Sleep(500);
	}

	return false;

}

void GameSession::GeneratePlacementId()
{
	UUID uuid;
	UuidCreate(&uuid);

	unsigned char* str = nullptr;
	UuidToStringA(&uuid, &str);

	mPlacementId.clear();
	mPlacementId = std::string((char*)str);

	RpcStringFreeA(&str);
}