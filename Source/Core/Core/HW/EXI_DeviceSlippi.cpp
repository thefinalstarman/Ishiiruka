// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/HW/EXI_DeviceSlippi.h"
#include "Core/Debugger/Debugger_SymbolMap.h"
#include "SlippiPlayback/SlippiPlayback.h"

#include <SlippiGame.h>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <semver/include/semver200.h>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility> // std::move

#ifdef _WIN32
#include <share.h>
#endif

#include "Common/CommonFuncs.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MemoryUtil.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"
#include "Core/HW/Memmap.h"

#include "Core/NetPlayClient.h"
#include "SlippiPlayback/SlippiPlayback.h"

#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/State.h"

// Not clean but idk a better way atm
//#ifndef LINUX_LOCAL_DEV
#include "DolphinWX/Frame.h"
#include "DolphinWX/Main.h"
//#endif

#define FRAME_INTERVAL 900
#define SLEEP_TIME_MS 8
#define WRITE_FILE_SLEEP_TIME_MS 85

//#define LOCAL_TESTING
//#define CREATE_DIFF_FILES

int32_t emod(int32_t a, int32_t b)
{
	assert(b != 0);
	int r = a % b;
	return r >= 0 ? r : r + std::abs(b);
}

static std::mutex mtx;
static std::mutex seekMtx;
static std::mutex diffMtx;
static std::unique_lock<std::mutex> processingLock(diffMtx);
static std::condition_variable condVar;
static std::condition_variable cv_waitingForTargetFrame;
static std::condition_variable cv_processingDiff;
static std::atomic<int> numDiffsProcessing(0);
static std::unordered_map<u8, std::string> slippi_names;
static std::unordered_map<u8, std::string> slippi_connect_codes;

extern std::unique_ptr<SlippiPlaybackStatus> g_playback_status;

template <typename T> bool isFutureReady(std::future<T> &t)
{
	return t.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

std::vector<u8> uint16ToVector(u16 num)
{
	u8 byte0 = num >> 8;
	u8 byte1 = num & 0xFF;

	return std::vector<u8>({byte0, byte1});
}

std::vector<u8> uint32ToVector(u32 num)
{
	u8 byte0 = num >> 24;
	u8 byte1 = (num & 0xFF0000) >> 16;
	u8 byte2 = (num & 0xFF00) >> 8;
	u8 byte3 = num & 0xFF;

	return std::vector<u8>({byte0, byte1, byte2, byte3});
}

std::vector<u8> int32ToVector(int32_t num)
{
	u8 byte0 = num >> 24;
	u8 byte1 = (num & 0xFF0000) >> 16;
	u8 byte2 = (num & 0xFF00) >> 8;
	u8 byte3 = num & 0xFF;

	return std::vector<u8>({byte0, byte1, byte2, byte3});
}

void appendWordToBuffer(std::vector<u8> *buf, u32 word)
{
	auto wordVector = uint32ToVector(word);
	buf->insert(buf->end(), wordVector.begin(), wordVector.end());
}

void appendHalfToBuffer(std::vector<u8> *buf, u16 word)
{
	auto halfVector = uint16ToVector(word);
	buf->insert(buf->end(), halfVector.begin(), halfVector.end());
}

std::string processDiff(std::vector<u8> iState, std::vector<u8> cState)
{
	INFO_LOG(SLIPPI, "Processing diff");
	numDiffsProcessing += 1;
	cv_processingDiff.notify_one();
	std::string diff = std::string();
	open_vcdiff::VCDiffEncoder encoder((char *)iState.data(), iState.size());
	encoder.Encode((char *)cState.data(), cState.size(), &diff);

	INFO_LOG(SLIPPI, "done processing");
	numDiffsProcessing -= 1;
	cv_processingDiff.notify_one();
	return diff;
}

CEXISlippi::CEXISlippi()
{
	INFO_LOG(SLIPPI, "EXI SLIPPI Constructor called.");

	user = std::make_unique<SlippiUser>();
	g_playback_status = std::make_unique<SlippiPlaybackStatus>();
	matchmaking = std::make_unique<SlippiMatchmaking>(user.get());
	gameFileLoader = std::make_unique<SlippiGameFileLoader>();
	replayComm = std::make_unique<SlippiReplayComm>();

	generator = std::default_random_engine(Common::Timer::GetTimeMs());

	// Loggers will check 5 bytes, make sure we own that memory
	m_read_queue.reserve(5);

	// Initialize local selections to empty
	localSelections.Reset();

	// Update user file and then listen for User
	user->UpdateFile();
	user->ListenForLogIn();

#ifdef CREATE_DIFF_FILES
	// MnMaAll.usd
	std::string origStr;
	std::string modifiedStr;
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.usd", origStr);
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll-new.usd",
	                       modifiedStr);
	std::vector<u8> orig(origStr.begin(), origStr.end());
	std::vector<u8> modified(modifiedStr.begin(), modifiedStr.end());
	auto diff = processDiff(orig, modified);
	File::WriteStringToFile(diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.usd.diff");
	File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnMaAll.usd.diff");

	// MnExtAll.usd
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.usd", origStr);
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll-new.usd", modifiedStr);
	orig = std::vector<u8>(origStr.begin(), origStr.end());
	modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
	diff = processDiff(orig, modified);
	File::WriteStringToFile(diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.usd.diff");
	File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnExtAll.usd.diff");

	// SdMenu.usd
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.usd", origStr);
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu-new.usd", modifiedStr);
	orig = std::vector<u8>(origStr.begin(), origStr.end());
	modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
	diff = processDiff(orig, modified);
	File::WriteStringToFile(diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.usd.diff");
	File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\SdMenu.usd.diff");

	// Japanese Files
	// MnMaAll.dat
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.dat", origStr);
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll-new.dat",
	                       modifiedStr);
	orig = std::vector<u8>(origStr.begin(), origStr.end());
	modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
	diff = processDiff(orig, modified);
	File::WriteStringToFile(diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll.dat.diff");
	File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnMaAll.dat.diff");

	// MnExtAll.dat
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.dat", origStr);
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll-new.dat", modifiedStr);
	orig = std::vector<u8>(origStr.begin(), origStr.end());
	modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
	diff = processDiff(orig, modified);
	File::WriteStringToFile(diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\CSS\\MnExtAll.dat.diff");
	File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\MnExtAll.dat.diff");

	// SdMenu.dat
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.dat", origStr);
	File::ReadFileToString("C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu-new.dat", modifiedStr);
	orig = std::vector<u8>(origStr.begin(), origStr.end());
	modified = std::vector<u8>(modifiedStr.begin(), modifiedStr.end());
	diff = processDiff(orig, modified);
	File::WriteStringToFile(diff, "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\SdMenu.dat.diff");
	File::WriteStringToFile(diff, "C:\\Dolphin\\IshiiDev\\Sys\\GameFiles\\GALE01\\SdMenu.dat.diff");

	// TEMP - Restore orig
	// std::string stateString;
	// decoder.Decode((char *)orig.data(), orig.size(), diff, &stateString);
	// File::WriteStringToFile(stateString,
	//                        "C:\\Users\\Jas\\Documents\\Melee\\Textures\\Slippi\\MainMenu\\MnMaAll-restored.usd");
#endif
}

CEXISlippi::~CEXISlippi()
{
	u8 empty[1];

	// Closes file gracefully to prevent file corruption when emulation
	// suddenly stops. This would happen often on netplay when the opponent
	// would close the emulation before the file successfully finished writing
	writeToFileAsync(&empty[0], 0, "close");
	writeThreadRunning = false;
	if (m_fileWriteThread.joinable())
	{
		m_fileWriteThread.join();
	}

	// Resets seek threads
	resetPlayback();

	localSelections.Reset();

	// g_playback_status = SlippiPlaybackStatus::SlippiPlaybackStatus();
}

void CEXISlippi::configureCommands(u8 *payload, u8 length)
{
	for (int i = 1; i < length; i += 3)
	{
		// Go through the receive commands payload and set up other commands
		u8 commandByte = payload[i];
		u32 commandPayloadSize = payload[i + 1] << 8 | payload[i + 2];
		payloadSizes[commandByte] = commandPayloadSize;
	}
}

void CEXISlippi::updateMetadataFields(u8 *payload, u32 length)
{
	if (length <= 0 || payload[0] != CMD_RECEIVE_POST_FRAME_UPDATE)
	{
		// Only need to update if this is a post frame update
		return;
	}

	// Keep track of last frame
	lastFrame = payload[1] << 24 | payload[2] << 16 | payload[3] << 8 | payload[4];

	// Keep track of character usage
	u8 playerIndex = payload[5];
	u8 internalCharacterId = payload[7];
	if (!characterUsage.count(playerIndex) || !characterUsage[playerIndex].count(internalCharacterId))
	{
		characterUsage[playerIndex][internalCharacterId] = 0;
	}
	characterUsage[playerIndex][internalCharacterId] += 1;
}

std::unordered_map<u8, std::string> CEXISlippi::getNetplayNames()
{
	std::unordered_map<u8, std::string> names;

	if (slippi_names.size())
	{
		names = slippi_names;
	}

	else if (netplay_client && netplay_client->IsConnected())
	{
		auto netplayPlayers = netplay_client->GetPlayers();
		for (auto it = netplayPlayers.begin(); it != netplayPlayers.end(); ++it)
		{
			auto player = *it;
			u8 portIndex = netplay_client->FindPlayerPad(player);
			if (portIndex < 0)
			{
				continue;
			}

			names[portIndex] = player->name;
		}
	}

	return names;
}

std::vector<u8> CEXISlippi::generateMetadata()
{
	std::vector<u8> metadata({'U', 8, 'm', 'e', 't', 'a', 'd', 'a', 't', 'a', '{'});

	// TODO: Abstract out UBJSON functions to make this cleaner

	// Add game start time
	uint8_t dateTimeStrLength = sizeof "2011-10-08T07:07:09Z";
	std::vector<char> dateTimeBuf(dateTimeStrLength);
	strftime(&dateTimeBuf[0], dateTimeStrLength, "%FT%TZ", gmtime(&gameStartTime));
	dateTimeBuf.pop_back(); // Removes the \0 from the back of string
	metadata.insert(metadata.end(), {'U', 7, 's', 't', 'a', 'r', 't', 'A', 't', 'S', 'U', (uint8_t)dateTimeBuf.size()});
	metadata.insert(metadata.end(), dateTimeBuf.begin(), dateTimeBuf.end());

	// Add game duration
	std::vector<u8> lastFrameToWrite = int32ToVector(lastFrame);
	metadata.insert(metadata.end(), {'U', 9, 'l', 'a', 's', 't', 'F', 'r', 'a', 'm', 'e', 'l'});
	metadata.insert(metadata.end(), lastFrameToWrite.begin(), lastFrameToWrite.end());

	// Add players elements to metadata, one per player index
	metadata.insert(metadata.end(), {'U', 7, 'p', 'l', 'a', 'y', 'e', 'r', 's', '{'});

	auto playerNames = getNetplayNames();

	for (auto it = characterUsage.begin(); it != characterUsage.end(); ++it)
	{
		auto playerIndex = it->first;
		auto playerCharacterUsage = it->second;

		metadata.push_back('U');
		std::string playerIndexStr = std::to_string(playerIndex);
		metadata.push_back((u8)playerIndexStr.length());
		metadata.insert(metadata.end(), playerIndexStr.begin(), playerIndexStr.end());
		metadata.push_back('{');

		// Add names element for this player
		metadata.insert(metadata.end(), {'U', 5, 'n', 'a', 'm', 'e', 's', '{'});

		if (playerNames.count(playerIndex))
		{
			auto playerName = playerNames[playerIndex];
			// Add netplay element for this player name
			metadata.insert(metadata.end(), {'U', 7, 'n', 'e', 't', 'p', 'l', 'a', 'y', 'S', 'U'});
			metadata.push_back((u8)playerName.length());
			metadata.insert(metadata.end(), playerName.begin(), playerName.end());
		}

		if (slippi_connect_codes.count(playerIndex))
		{
			auto connectCode = slippi_connect_codes[playerIndex];
			// Add connection code element for this player name
			metadata.insert(metadata.end(), {'U', 4, 'c', 'o', 'd', 'e', 'S', 'U'});
			metadata.push_back((u8)connectCode.length());
			metadata.insert(metadata.end(), connectCode.begin(), connectCode.end());
		}

		metadata.push_back('}'); // close names

		// Add character element for this player
		metadata.insert(metadata.end(), {'U', 10, 'c', 'h', 'a', 'r', 'a', 'c', 't', 'e', 'r', 's', '{'});
		for (auto it2 = playerCharacterUsage.begin(); it2 != playerCharacterUsage.end(); ++it2)
		{
			metadata.push_back('U');
			std::string internalCharIdStr = std::to_string(it2->first);
			metadata.push_back((u8)internalCharIdStr.length());
			metadata.insert(metadata.end(), internalCharIdStr.begin(), internalCharIdStr.end());

			metadata.push_back('l');
			std::vector<u8> frameCount = uint32ToVector(it2->second);
			metadata.insert(metadata.end(), frameCount.begin(), frameCount.end());
		}
		metadata.push_back('}'); // close characters

		metadata.push_back('}'); // close player
	}
	metadata.push_back('}');

	// Indicate this was played on dolphin
	metadata.insert(metadata.end(),
	                {'U', 8, 'p', 'l', 'a', 'y', 'e', 'd', 'O', 'n', 'S', 'U', 7, 'd', 'o', 'l', 'p', 'h', 'i', 'n'});

	metadata.push_back('}');
	return metadata;
}

void CEXISlippi::writeToFileAsync(u8 *payload, u32 length, std::string fileOption)
{
	if (fileOption == "create" && !writeThreadRunning)
	{
		WARN_LOG(SLIPPI, "Creating file write thread...");
		writeThreadRunning = true;
		m_fileWriteThread = std::thread(&CEXISlippi::FileWriteThread, this);
	}

	if (!writeThreadRunning)
	{
		return;
	}

	std::vector<u8> payloadData;
	payloadData.insert(payloadData.end(), payload, payload + length);

	auto writeMsg = std::make_unique<WriteMessage>();
	writeMsg->data = payloadData;
	writeMsg->operation = fileOption;

	fileWriteQueue.Push(std::move(writeMsg));
}

void CEXISlippi::FileWriteThread(void)
{
	while (writeThreadRunning || !fileWriteQueue.Empty())
	{
		// Process all messages
		while (!fileWriteQueue.Empty())
		{
			writeToFile(std::move(fileWriteQueue.Front()));
			fileWriteQueue.Pop();

			Common::SleepCurrentThread(0);
		}

		Common::SleepCurrentThread(WRITE_FILE_SLEEP_TIME_MS);
	}
}

void CEXISlippi::writeToFile(std::unique_ptr<WriteMessage> msg)
{
	if (!msg)
	{
		ERROR_LOG(SLIPPI, "Unexpected error: write message is falsy.");
		return;
	}

	u8 *payload = &msg->data[0];
	u32 length = (u32)msg->data.size();
	std::string fileOption = msg->operation;

	std::vector<u8> dataToWrite;
	if (fileOption == "create")
	{
		// If the game sends over option 1 that means a file should be created
		createNewFile();

		// Start ubjson file and prepare the "raw" element that game
		// data output will be dumped into. The size of the raw output will
		// be initialized to 0 until all of the data has been received
		std::vector<u8> headerBytes({'{', 'U', 3, 'r', 'a', 'w', '[', '$', 'U', '#', 'l', 0, 0, 0, 0});
		dataToWrite.insert(dataToWrite.end(), headerBytes.begin(), headerBytes.end());

		// Used to keep track of how many bytes have been written to the file
		writtenByteCount = 0;

		// Used to track character usage (sheik/zelda)
		characterUsage.clear();

		// Reset lastFrame
		lastFrame = Slippi::GAME_FIRST_FRAME;

		// Get display names and connection codes from slippi netplay client
		if (slippi_netplay)
		{
			auto matchInfo = slippi_netplay->GetMatchInfo();

			SlippiPlayerSelections lps = matchInfo->localPlayerSelections;
			SlippiPlayerSelections rps = matchInfo->remotePlayerSelections;

			auto isDecider = slippi_netplay->IsDecider();
			int local_port = isDecider ? 0 : 1;
			int remote_port = isDecider ? 1 : 0;

			slippi_names[local_port] = lps.playerName;
			slippi_connect_codes[local_port] = lps.connectCode;
			slippi_names[remote_port] = rps.playerName;
			slippi_connect_codes[remote_port] = rps.connectCode;
		}
	}

	// If no file, do nothing
	if (!m_file)
	{
		return;
	}

	// Update fields relevant to generating metadata at the end
	updateMetadataFields(payload, length);

	// Add the payload to data to write
	dataToWrite.insert(dataToWrite.end(), payload, payload + length);
	writtenByteCount += length;

	// If we are going to close the file, generate data to complete the UBJSON file
	if (fileOption == "close")
	{
		// This option indicates we are done sending over body
		std::vector<u8> closingBytes = generateMetadata();
		closingBytes.push_back('}');
		dataToWrite.insert(dataToWrite.end(), closingBytes.begin(), closingBytes.end());

		// Reset display names and connect codes retrieved from netplay client
		slippi_names.clear();
		slippi_connect_codes.clear();
	}

	// Write data to file
	bool result = m_file.WriteBytes(&dataToWrite[0], dataToWrite.size());
	if (!result)
	{
		ERROR_LOG(EXPANSIONINTERFACE, "Failed to write data to file.");
	}

	// If file should be closed, close it
	if (fileOption == "close")
	{
		// Write the number of bytes for the raw output
		std::vector<u8> sizeBytes = uint32ToVector(writtenByteCount);
		m_file.Seek(11, 0);
		m_file.WriteBytes(&sizeBytes[0], sizeBytes.size());

		// Close file
		closeFile();
	}
}

void CEXISlippi::createNewFile()
{
	if (m_file)
	{
		// If there's already a file open, close that one
		closeFile();
	}

	std::string dirpath = File::GetExeDirectory();
	File::CreateDir(dirpath + DIR_SEP + "Slippi");
	std::string filepath = dirpath + DIR_SEP + generateFileName();

	INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Creating new replay file %s", filepath.c_str());

#ifdef _WIN32
	m_file = File::IOFile(filepath, "wb", _SH_DENYWR);
#else
	m_file = File::IOFile(filepath, "wb");
#endif
}

std::string CEXISlippi::generateFileName()
{
	// Add game start time
	uint8_t dateTimeStrLength = sizeof "20171015T095717";
	std::vector<char> dateTimeBuf(dateTimeStrLength);
	strftime(&dateTimeBuf[0], dateTimeStrLength, "%Y%m%dT%H%M%S", localtime(&gameStartTime));

	std::string str(&dateTimeBuf[0]);
	return StringFromFormat("Slippi/Game_%s.slp", str.c_str());
}

void CEXISlippi::closeFile()
{
	if (!m_file)
	{
		// If we have no file or payload is not game end, do nothing
		return;
	}

	// If this is the end of the game end payload, reset the file so that we create a new one
	m_file.Close();
	m_file = nullptr;
}

void CEXISlippi::prepareGameInfo(u8 *payload)
{
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game)
	{
		// Do nothing if we don't have a game loaded
		return;
	}

	if (!m_current_game->AreSettingsLoaded())
	{
		m_read_queue.push_back(0);
		return;
	}

	// Return success code
	m_read_queue.push_back(1);

	// Prepare playback savestate payload
	playbackSavestatePayload.clear();
	appendWordToBuffer(&playbackSavestatePayload, 0); // This space will be used to set frame index
	int bkpPos = 0;
	while ((*(u32 *)(&payload[bkpPos * 8])) != 0)
	{
		bkpPos += 1;
	}
	playbackSavestatePayload.insert(playbackSavestatePayload.end(), payload, payload + (bkpPos * 8 + 4));

	Slippi::GameSettings *settings = m_current_game->GetSettings();

	// Start in Fast Forward if this is mirrored
	auto replayCommSettings = replayComm->getSettings();
	if (!isHardFFW)
		isHardFFW = replayCommSettings.mode == "mirror";
	lastFFWFrame = INT_MIN;

	// Build a word containing the stage and the presence of the characters
	u32 randomSeed = settings->randomSeed;
	appendWordToBuffer(&m_read_queue, randomSeed);

	// This is kinda dumb but we need to handle the case where a player transforms
	// into sheik/zelda immediately. This info is not stored in the game info header
	// and so let's overwrite those values
	int player1Pos = 24; // This is the index of the first players character info
	std::array<uint32_t, Slippi::GAME_INFO_HEADER_SIZE> gameInfoHeader = settings->header;
	for (int i = 0; i < 4; i++)
	{
		// check if this player is actually in the game
		bool playerExists = m_current_game->DoesPlayerExist(i);
		if (!playerExists)
		{
			continue;
		}

		// check if the player is playing sheik or zelda
		uint8_t externalCharId = settings->players[i].characterId;
		if (externalCharId != 0x12 && externalCharId != 0x13)
		{
			continue;
		}

		// this is the position in the array that this player's character info is stored
		int pos = player1Pos + (9 * i);

		// here we have determined the player is playing sheik or zelda...
		// at this point let's overwrite the player's character with the one
		// that they are playing
		gameInfoHeader[pos] &= 0x00FFFFFF;
		gameInfoHeader[pos] |= externalCharId << 24;
	}

	// Write entire header to game
	for (int i = 0; i < Slippi::GAME_INFO_HEADER_SIZE; i++)
	{
		appendWordToBuffer(&m_read_queue, gameInfoHeader[i]);
	}

	// Write UCF toggles
	std::array<uint32_t, Slippi::UCF_TOGGLE_SIZE> ucfToggles = settings->ucfToggles;
	for (int i = 0; i < Slippi::UCF_TOGGLE_SIZE; i++)
	{
		appendWordToBuffer(&m_read_queue, ucfToggles[i]);
	}

	// Write nametags
	for (int i = 0; i < 4; i++)
	{
		auto player = settings->players[i];
		for (int j = 0; j < Slippi::NAMETAG_SIZE; j++)
		{
			appendHalfToBuffer(&m_read_queue, player.nametag[j]);
		}
	}

	// Write PAL byte
	m_read_queue.push_back(settings->isPAL);

	// Get replay version numbers
	auto replayVersion = m_current_game->GetVersion();
	auto majorVersion = replayVersion[0];
	auto minorVersion = replayVersion[1];

	// Write PS pre-load byte
	auto shouldPreloadPs = majorVersion > 1 || (majorVersion == 1 && minorVersion > 2);
	m_read_queue.push_back(shouldPreloadPs);

	// Write PS Frozen byte
	m_read_queue.push_back(settings->isFrozenPS);

	// Return the size of the gecko code list
	prepareGeckoList();
	appendWordToBuffer(&m_read_queue, (u32)geckoList.size());

	// Initialize frame sequence index value for reading rollbacks
	frameSeqIdx = 0;

	if (replayCommSettings.rollbackDisplayMethod != "off")
	{
		// Prepare savestates
		availableSavestates.clear();
		activeSavestates.clear();

		// Prepare savestates for online play
		for (int i = 0; i < ROLLBACK_MAX_FRAMES; i++)
		{
			availableSavestates.push_back(std::make_unique<SlippiSavestate>());
		}
	}
	else
	{
		// Prepare savestates
		availableSavestates.clear();
		activeSavestates.clear();

		// Add savestate for testing
		availableSavestates.push_back(std::make_unique<SlippiSavestate>());
	}

	// Reset playback frame to begining
	g_playback_status->currentPlaybackFrame = Slippi::GAME_FIRST_FRAME;

	// Initialize replay related threads
	if (replayCommSettings.mode == "normal" || replayCommSettings.mode == "queue")
	{
		g_playback_status->shouldRunThreads = true;
		m_savestateThread = std::thread(&CEXISlippi::SavestateThread, this);
		m_seekThread = std::thread(&CEXISlippi::SeekThread, this);
	}
}

void CEXISlippi::prepareGeckoList()
{
	// TODO: How do I move this somewhere else?
	// This contains all of the codes required to play legacy replays (UCF, PAL, Frz Stadium)
	static std::vector<uint8_t> defaultCodeList = {
	    0xC2, 0x0C, 0x9A, 0x44, 0x00, 0x00, 0x00, 0x2F, // #External/UCF + Arduino Toggle UI/UCF/UCF 0.74
	                                                    // Dashback - Check for Toggle.asm
	    0xD0, 0x1F, 0x00, 0x2C, 0x88, 0x9F, 0x06, 0x18, 0x38, 0x62, 0xF2, 0x28, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03,
	    0x00, 0x01, 0x41, 0x82, 0x00, 0x14, 0x38, 0x62, 0xF2, 0x2C, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03, 0x00, 0x01,
	    0x40, 0x82, 0x01, 0x50, 0x7C, 0x08, 0x02, 0xA6, 0x90, 0x01, 0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81,
	    0x00, 0x08, 0x48, 0x00, 0x01, 0x21, 0x7F, 0xC8, 0x02, 0xA6, 0xC0, 0x3F, 0x08, 0x94, 0xC0, 0x5E, 0x00, 0x00,
	    0xFC, 0x01, 0x10, 0x40, 0x40, 0x82, 0x01, 0x18, 0x80, 0x8D, 0xAE, 0xB4, 0xC0, 0x3F, 0x06, 0x20, 0xFC, 0x20,
	    0x0A, 0x10, 0xC0, 0x44, 0x00, 0x3C, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x80, 0x01, 0x00, 0x88, 0x7F, 0x06, 0x70,
	    0x2C, 0x03, 0x00, 0x02, 0x40, 0x80, 0x00, 0xF4, 0x88, 0x7F, 0x22, 0x1F, 0x54, 0x60, 0x07, 0x39, 0x40, 0x82,
	    0x00, 0xE8, 0x3C, 0x60, 0x80, 0x4C, 0x60, 0x63, 0x1F, 0x78, 0x8B, 0xA3, 0x00, 0x01, 0x38, 0x7D, 0xFF, 0xFE,
	    0x88, 0x9F, 0x06, 0x18, 0x48, 0x00, 0x00, 0x8D, 0x7C, 0x7C, 0x1B, 0x78, 0x7F, 0xA3, 0xEB, 0x78, 0x88, 0x9F,
	    0x06, 0x18, 0x48, 0x00, 0x00, 0x7D, 0x7C, 0x7C, 0x18, 0x50, 0x7C, 0x63, 0x19, 0xD6, 0x2C, 0x03, 0x15, 0xF9,
	    0x40, 0x81, 0x00, 0xB0, 0x38, 0x00, 0x00, 0x01, 0x90, 0x1F, 0x23, 0x58, 0x90, 0x1F, 0x23, 0x40, 0x80, 0x9F,
	    0x00, 0x04, 0x2C, 0x04, 0x00, 0x0A, 0x40, 0xA2, 0x00, 0x98, 0x88, 0x7F, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x01,
	    0x3D, 0x80, 0x80, 0x03, 0x61, 0x8C, 0x41, 0x8C, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x2C, 0x03,
	    0x00, 0x00, 0x41, 0x82, 0x00, 0x78, 0x80, 0x83, 0x00, 0x2C, 0x80, 0x84, 0x1E, 0xCC, 0xC0, 0x3F, 0x00, 0x2C,
	    0xD0, 0x24, 0x00, 0x18, 0xC0, 0x5E, 0x00, 0x04, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x81, 0x00, 0x0C, 0x38, 0x60,
	    0x00, 0x80, 0x48, 0x00, 0x00, 0x08, 0x38, 0x60, 0x00, 0x7F, 0x98, 0x64, 0x00, 0x06, 0x48, 0x00, 0x00, 0x48,
	    0x7C, 0x85, 0x23, 0x78, 0x38, 0x63, 0xFF, 0xFF, 0x2C, 0x03, 0x00, 0x00, 0x40, 0x80, 0x00, 0x08, 0x38, 0x63,
	    0x00, 0x05, 0x3C, 0x80, 0x80, 0x46, 0x60, 0x84, 0xB1, 0x08, 0x1C, 0x63, 0x00, 0x30, 0x7C, 0x84, 0x1A, 0x14,
	    0x1C, 0x65, 0x00, 0x0C, 0x7C, 0x84, 0x1A, 0x14, 0x88, 0x64, 0x00, 0x02, 0x7C, 0x63, 0x07, 0x74, 0x4E, 0x80,
	    0x00, 0x20, 0x4E, 0x80, 0x00, 0x21, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xBA, 0x81, 0x00, 0x08,
	    0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x09,
	    0x98, 0xA4, 0x00, 0x00, 0x00, 0x2B, // #External/UCF + Arduino Toggle UI/UCF/UCF
	                                        // 0.74 Shield Drop - Check for Toggle.asm
	    0x7C, 0x08, 0x02, 0xA6, 0x90, 0x01, 0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81, 0x00, 0x08, 0x7C, 0x7E,
	    0x1B, 0x78, 0x83, 0xFE, 0x00, 0x2C, 0x48, 0x00, 0x01, 0x01, 0x7F, 0xA8, 0x02, 0xA6, 0x88, 0x9F, 0x06, 0x18,
	    0x38, 0x62, 0xF2, 0x28, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03, 0x00, 0x01, 0x41, 0x82, 0x00, 0x14, 0x38, 0x62,
	    0xF2, 0x30, 0x7C, 0x63, 0x20, 0xAE, 0x2C, 0x03, 0x00, 0x01, 0x40, 0x82, 0x00, 0xF8, 0xC0, 0x3F, 0x06, 0x3C,
	    0x80, 0x6D, 0xAE, 0xB4, 0xC0, 0x03, 0x03, 0x14, 0xFC, 0x01, 0x00, 0x40, 0x40, 0x81, 0x00, 0xE4, 0xC0, 0x3F,
	    0x06, 0x20, 0x48, 0x00, 0x00, 0x71, 0xD0, 0x21, 0x00, 0x90, 0xC0, 0x3F, 0x06, 0x24, 0x48, 0x00, 0x00, 0x65,
	    0xC0, 0x41, 0x00, 0x90, 0xEC, 0x42, 0x00, 0xB2, 0xEC, 0x21, 0x00, 0x72, 0xEC, 0x21, 0x10, 0x2A, 0xC0, 0x5D,
	    0x00, 0x0C, 0xFC, 0x01, 0x10, 0x40, 0x41, 0x80, 0x00, 0xB4, 0x88, 0x9F, 0x06, 0x70, 0x2C, 0x04, 0x00, 0x03,
	    0x40, 0x81, 0x00, 0xA8, 0xC0, 0x1D, 0x00, 0x10, 0xC0, 0x3F, 0x06, 0x24, 0xFC, 0x00, 0x08, 0x40, 0x40, 0x80,
	    0x00, 0x98, 0xBA, 0x81, 0x00, 0x08, 0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6,
	    0x80, 0x61, 0x00, 0x1C, 0x83, 0xE1, 0x00, 0x14, 0x38, 0x21, 0x00, 0x18, 0x38, 0x63, 0x00, 0x08, 0x7C, 0x68,
	    0x03, 0xA6, 0x4E, 0x80, 0x00, 0x20, 0xFC, 0x00, 0x0A, 0x10, 0xC0, 0x3D, 0x00, 0x00, 0xEC, 0x00, 0x00, 0x72,
	    0xC0, 0x3D, 0x00, 0x04, 0xEC, 0x00, 0x08, 0x28, 0xFC, 0x00, 0x00, 0x1E, 0xD8, 0x01, 0x00, 0x80, 0x80, 0x61,
	    0x00, 0x84, 0x38, 0x63, 0x00, 0x02, 0x3C, 0x00, 0x43, 0x30, 0xC8, 0x5D, 0x00, 0x14, 0x6C, 0x63, 0x80, 0x00,
	    0x90, 0x01, 0x00, 0x80, 0x90, 0x61, 0x00, 0x84, 0xC8, 0x21, 0x00, 0x80, 0xEC, 0x01, 0x10, 0x28, 0xC0, 0x3D,
	    0x00, 0x00, 0xEC, 0x20, 0x08, 0x24, 0x4E, 0x80, 0x00, 0x20, 0x4E, 0x80, 0x00, 0x21, 0x42, 0xA0, 0x00, 0x00,
	    0x37, 0x27, 0x00, 0x00, 0x43, 0x30, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0xBF, 0x4C, 0xCC, 0xCD, 0x43, 0x30,
	    0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x7F, 0xC3, 0xF3, 0x78, 0x7F, 0xE4, 0xFB, 0x78, 0xBA, 0x81, 0x00, 0x08,
	    0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08, 0x03, 0xA6, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0xC2, 0x16, 0xE7, 0x50, 0x00, 0x00, 0x00,
	    0x33, // #Common/StaticPatches/ToggledStaticOverwrites.asm
	    0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x14, 0x48, 0x00, 0x00, 0x75, 0x7C, 0x68,
	    0x02, 0xA6, 0x48, 0x00, 0x01, 0x3D, 0x48, 0x00, 0x00, 0x14, 0x48, 0x00, 0x00, 0x95, 0x7C, 0x68, 0x02, 0xA6,
	    0x48, 0x00, 0x01, 0x2D, 0x48, 0x00, 0x00, 0x04, 0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82,
	    0x00, 0x14, 0x48, 0x00, 0x00, 0xB9, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x11, 0x48, 0x00, 0x00, 0x10,
	    0x48, 0x00, 0x00, 0xC9, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x01, 0x01, 0x88, 0x62, 0xF2, 0x3C, 0x2C, 0x03,
	    0x00, 0x00, 0x41, 0x82, 0x00, 0x14, 0x48, 0x00, 0x00, 0xD1, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x00, 0xE9,
	    0x48, 0x00, 0x01, 0x04, 0x48, 0x00, 0x00, 0xD1, 0x7C, 0x68, 0x02, 0xA6, 0x48, 0x00, 0x00, 0xD9, 0x48, 0x00,
	    0x00, 0xF4, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x3C, 0xE4, 0xD4, 0x00, 0x24, 0x04, 0x64, 0x80, 0x07, 0x96, 0xE0,
	    0x60, 0x00, 0x00, 0x00, 0x80, 0x2B, 0x7E, 0x54, 0x48, 0x00, 0x00, 0x88, 0x80, 0x2B, 0x80, 0x8C, 0x48, 0x00,
	    0x00, 0x84, 0x80, 0x12, 0x39, 0xA8, 0x60, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21,
	    0x80, 0x3C, 0xE4, 0xD4, 0x00, 0x20, 0x00, 0x00, 0x80, 0x07, 0x96, 0xE0, 0x3A, 0x40, 0x00, 0x01, 0x80, 0x2B,
	    0x7E, 0x54, 0x88, 0x7F, 0x22, 0x40, 0x80, 0x2B, 0x80, 0x8C, 0x2C, 0x03, 0x00, 0x02, 0x80, 0x10, 0xFC, 0x48,
	    0x90, 0x05, 0x21, 0xDC, 0x80, 0x10, 0xFB, 0x68, 0x90, 0x05, 0x21, 0xDC, 0x80, 0x12, 0x39, 0xA8, 0x90, 0x1F,
	    0x1A, 0x5C, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x46, 0x10, 0x48, 0x00, 0x00, 0x4C,
	    0x80, 0x1D, 0x47, 0x24, 0x48, 0x00, 0x00, 0x3C, 0x80, 0x1D, 0x46, 0x0C, 0x80, 0x9F, 0x00, 0xEC, 0xFF, 0xFF,
	    0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21, 0x80, 0x1D, 0x46, 0x10, 0x38, 0x83, 0x7F, 0x9C, 0x80, 0x1D, 0x47, 0x24,
	    0x88, 0x1B, 0x00, 0xC4, 0x80, 0x1D, 0x46, 0x0C, 0x3C, 0x60, 0x80, 0x3B, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80,
	    0x00, 0x21, 0x80, 0x1D, 0x45, 0xFC, 0x48, 0x00, 0x09, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x80, 0x00, 0x21,
	    0x80, 0x1D, 0x45, 0xFC, 0x40, 0x80, 0x09, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0x38, 0xA3, 0xFF, 0xFC, 0x84, 0x65,
	    0x00, 0x04, 0x2C, 0x03, 0xFF, 0xFF, 0x41, 0x82, 0x00, 0x10, 0x84, 0x85, 0x00, 0x04, 0x90, 0x83, 0x00, 0x00,
	    0x4B, 0xFF, 0xFF, 0xEC, 0x4E, 0x80, 0x00, 0x20, 0x3C, 0x60, 0x80, 0x00, 0x3C, 0x80, 0x00, 0x3B, 0x60, 0x84,
	    0x72, 0x2C, 0x3D, 0x80, 0x80, 0x32, 0x61, 0x8C, 0x8F, 0x50, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21,
	    0x3C, 0x60, 0x80, 0x17, 0x3C, 0x80, 0x80, 0x17, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x1D, 0x14, 0xC8, 0x00, 0x00,
	    0x00, 0x04, // #Common/Preload Stadium
	                // Transformations/Handlers/Init
	                // isLoaded Bool.asm
	    0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x0C, 0x38, 0x60, 0x00, 0x00, 0x98, 0x7F,
	    0x00, 0xF0, 0x3B, 0xA0, 0x00, 0x01, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x1D, 0x45, 0xEC,
	    0x00, 0x00, 0x00, 0x1B, // #Common/Preload Stadium
	                            // Transformations/Handlers/Load
	                            // Transformation.asm
	    0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0xC4, 0x88, 0x7F, 0x00, 0xF0, 0x2C, 0x03,
	    0x00, 0x00, 0x40, 0x82, 0x00, 0xB8, 0x38, 0x60, 0x00, 0x04, 0x3D, 0x80, 0x80, 0x38, 0x61, 0x8C, 0x05, 0x80,
	    0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x54, 0x60, 0x10, 0x3A, 0xA8, 0x7F, 0x00, 0xE2, 0x3C, 0x80,
	    0x80, 0x3B, 0x60, 0x84, 0x7F, 0x9C, 0x7C, 0x84, 0x00, 0x2E, 0x7C, 0x03, 0x20, 0x00, 0x41, 0x82, 0xFF, 0xD4,
	    0x90, 0x9F, 0x00, 0xEC, 0x2C, 0x04, 0x00, 0x03, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x00, 0x48, 0x00,
	    0x00, 0x34, 0x2C, 0x04, 0x00, 0x04, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x01, 0x48, 0x00, 0x00, 0x24,
	    0x2C, 0x04, 0x00, 0x09, 0x40, 0x82, 0x00, 0x0C, 0x38, 0x80, 0x00, 0x02, 0x48, 0x00, 0x00, 0x14, 0x2C, 0x04,
	    0x00, 0x06, 0x40, 0x82, 0x00, 0x00, 0x38, 0x80, 0x00, 0x03, 0x48, 0x00, 0x00, 0x04, 0x3C, 0x60, 0x80, 0x3E,
	    0x60, 0x63, 0x12, 0x48, 0x54, 0x80, 0x10, 0x3A, 0x7C, 0x63, 0x02, 0x14, 0x80, 0x63, 0x03, 0xD8, 0x80, 0x9F,
	    0x00, 0xCC, 0x38, 0xBF, 0x00, 0xC8, 0x3C, 0xC0, 0x80, 0x1D, 0x60, 0xC6, 0x42, 0x20, 0x38, 0xE0, 0x00, 0x00,
	    0x3D, 0x80, 0x80, 0x01, 0x61, 0x8C, 0x65, 0x80, 0x7D, 0x89, 0x03, 0xA6, 0x4E, 0x80, 0x04, 0x21, 0x38, 0x60,
	    0x00, 0x01, 0x98, 0x7F, 0x00, 0xF0, 0x80, 0x7F, 0x00, 0xD8, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0xC2, 0x1D, 0x4F, 0x14, 0x00, 0x00, 0x00, 0x04, // #Common/Preload
	                                                    // Stadium
	                                                    // Transformations/Handlers/Reset
	                                                    // isLoaded.asm
	    0x88, 0x62, 0xF2, 0x38, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x0C, 0x38, 0x60, 0x00, 0x00, 0x98, 0x7F,
	    0x00, 0xF0, 0x80, 0x6D, 0xB2, 0xD8, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x06, 0x8F, 0x30,
	    0x00, 0x00, 0x00, 0x9D, // #Common/PAL/Handlers/Character DAT
	                            // Patcher.asm
	    0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x04, 0xD4, 0x7C, 0x08, 0x02, 0xA6, 0x90, 0x01,
	    0x00, 0x04, 0x94, 0x21, 0xFF, 0x50, 0xBE, 0x81, 0x00, 0x08, 0x83, 0xFE, 0x01, 0x0C, 0x83, 0xFF, 0x00, 0x08,
	    0x3B, 0xFF, 0xFF, 0xE0, 0x80, 0x7D, 0x00, 0x00, 0x2C, 0x03, 0x00, 0x1B, 0x40, 0x80, 0x04, 0x9C, 0x48, 0x00,
	    0x00, 0x71, 0x48, 0x00, 0x00, 0xA9, 0x48, 0x00, 0x00, 0xB9, 0x48, 0x00, 0x01, 0x51, 0x48, 0x00, 0x01, 0x79,
	    0x48, 0x00, 0x01, 0x79, 0x48, 0x00, 0x02, 0x29, 0x48, 0x00, 0x02, 0x39, 0x48, 0x00, 0x02, 0x81, 0x48, 0x00,
	    0x02, 0xF9, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11, 0x48, 0x00, 0x03, 0x11,
	    0x48, 0x00, 0x03, 0x21, 0x48, 0x00, 0x03, 0x21, 0x48, 0x00, 0x03, 0x89, 0x48, 0x00, 0x03, 0x89, 0x48, 0x00,
	    0x03, 0x91, 0x48, 0x00, 0x03, 0x91, 0x48, 0x00, 0x03, 0xA9, 0x48, 0x00, 0x03, 0xA9, 0x48, 0x00, 0x03, 0xB9,
	    0x48, 0x00, 0x03, 0xB9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00, 0x03, 0xC9, 0x48, 0x00,
	    0x04, 0x29, 0x7C, 0x88, 0x02, 0xA6, 0x1C, 0x63, 0x00, 0x04, 0x7C, 0x84, 0x1A, 0x14, 0x80, 0xA4, 0x00, 0x00,
	    0x54, 0xA5, 0x01, 0xBA, 0x7C, 0xA4, 0x2A, 0x14, 0x80, 0x65, 0x00, 0x00, 0x80, 0x85, 0x00, 0x04, 0x2C, 0x03,
	    0x00, 0xFF, 0x41, 0x82, 0x00, 0x14, 0x7C, 0x63, 0xFA, 0x14, 0x90, 0x83, 0x00, 0x00, 0x38, 0xA5, 0x00, 0x08,
	    0x4B, 0xFF, 0xFF, 0xE4, 0x48, 0x00, 0x03, 0xF0, 0x00, 0x00, 0x33, 0x44, 0x3F, 0x54, 0x7A, 0xE1, 0x00, 0x00,
	    0x33, 0x60, 0x42, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x37, 0x9C, 0x42, 0x92, 0x00, 0x00,
	    0x00, 0x00, 0x39, 0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39, 0x0C, 0x40, 0x86, 0x66, 0x66, 0x00, 0x00,
	    0x39, 0x10, 0x3D, 0xEA, 0x0E, 0xA1, 0x00, 0x00, 0x39, 0x28, 0x41, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x3C, 0x04,
	    0x2C, 0x01, 0x48, 0x0C, 0x00, 0x00, 0x47, 0x20, 0x1B, 0x96, 0x80, 0x13, 0x00, 0x00, 0x47, 0x34, 0x1B, 0x96,
	    0x80, 0x13, 0x00, 0x00, 0x47, 0x3C, 0x04, 0x00, 0x00, 0x09, 0x00, 0x00, 0x4A, 0x40, 0x2C, 0x00, 0x68, 0x11,
	    0x00, 0x00, 0x4A, 0x4C, 0x28, 0x1B, 0x00, 0x13, 0x00, 0x00, 0x4A, 0x50, 0x0D, 0x00, 0x01, 0x0B, 0x00, 0x00,
	    0x4A, 0x54, 0x2C, 0x80, 0x68, 0x11, 0x00, 0x00, 0x4A, 0x60, 0x28, 0x1B, 0x00, 0x13, 0x00, 0x00, 0x4A, 0x64,
	    0x0D, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x4B, 0x24, 0x2C, 0x00, 0x68, 0x0D, 0x00, 0x00, 0x4B, 0x30, 0x0F, 0x10,
	    0x40, 0x13, 0x00, 0x00, 0x4B, 0x38, 0x2C, 0x80, 0x38, 0x0D, 0x00, 0x00, 0x4B, 0x44, 0x0F, 0x10, 0x40, 0x13,
	    0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x38, 0x0C, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x4E, 0xF8, 0x2C, 0x00,
	    0x38, 0x03, 0x00, 0x00, 0x4F, 0x08, 0x0F, 0x80, 0x00, 0x0B, 0x00, 0x00, 0x4F, 0x0C, 0x2C, 0x80, 0x20, 0x03,
	    0x00, 0x00, 0x4F, 0x1C, 0x0F, 0x80, 0x00, 0x0B, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x4D, 0x10, 0x3F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x4D, 0x70, 0x42, 0x94, 0x00, 0x00, 0x00, 0x00, 0x4D, 0xD4,
	    0x41, 0x90, 0x00, 0x00, 0x00, 0x00, 0x4D, 0xE0, 0x41, 0x90, 0x00, 0x00, 0x00, 0x00, 0x83, 0xAC, 0x2C, 0x00,
	    0x00, 0x09, 0x00, 0x00, 0x83, 0xB8, 0x34, 0x8C, 0x80, 0x11, 0x00, 0x00, 0x84, 0x00, 0x34, 0x8C, 0x80, 0x11,
	    0x00, 0x00, 0x84, 0x30, 0x05, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x84, 0x38, 0x04, 0x1A, 0x05, 0x00, 0x00, 0x00,
	    0x84, 0x44, 0x05, 0x00, 0x00, 0x8B, 0x00, 0x00, 0x84, 0xDC, 0x05, 0x78, 0x05, 0x78, 0x00, 0x00, 0x85, 0xB8,
	    0x10, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x85, 0xC0, 0x03, 0xE8, 0x01, 0xF4, 0x00, 0x00, 0x85, 0xCC, 0x10, 0x00,
	    0x01, 0x0B, 0x00, 0x00, 0x85, 0xD4, 0x03, 0x84, 0x03, 0xE8, 0x00, 0x00, 0x85, 0xE0, 0x10, 0x00, 0x01, 0x0B,
	    0x00, 0x00, 0x88, 0x18, 0x0B, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x88, 0x2C, 0x0B, 0x00, 0x01, 0x0B, 0x00, 0x00,
	    0x88, 0xF8, 0x04, 0x1A, 0x0B, 0xB8, 0x00, 0x00, 0x89, 0x3C, 0x04, 0x1A, 0x0B, 0xB8, 0x00, 0x00, 0x89, 0x80,
	    0x04, 0x1A, 0x0B, 0xB8, 0x00, 0x00, 0x89, 0xE0, 0x04, 0xFE, 0xF7, 0x04, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x36, 0xCC, 0x42, 0xEC, 0x00, 0x00, 0x00, 0x00, 0x37, 0xC4, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,
	    0x00, 0x00, 0x34, 0x68, 0x3F, 0x66, 0x66, 0x66, 0x00, 0x00, 0x39, 0xD8, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x00,
	    0x3A, 0x44, 0xB4, 0x99, 0x00, 0x11, 0x00, 0x00, 0x3A, 0x48, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3A, 0x58,
	    0xB4, 0x99, 0x00, 0x11, 0x00, 0x00, 0x3A, 0x5C, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3A, 0x6C, 0xB4, 0x99,
	    0x00, 0x11, 0x00, 0x00, 0x3A, 0x70, 0x1B, 0x8C, 0x00, 0x8F, 0x00, 0x00, 0x3B, 0x30, 0x44, 0x0C, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x45, 0xC8, 0x2C, 0x01, 0x50, 0x10, 0x00, 0x00, 0x45, 0xD4, 0x2D, 0x19,
	    0x80, 0x13, 0x00, 0x00, 0x45, 0xDC, 0x2C, 0x80, 0xB0, 0x10, 0x00, 0x00, 0x45, 0xE8, 0x2D, 0x19, 0x80, 0x13,
	    0x00, 0x00, 0x49, 0xC4, 0x2C, 0x00, 0x68, 0x0A, 0x00, 0x00, 0x49, 0xD0, 0x28, 0x1B, 0x80, 0x13, 0x00, 0x00,
	    0x49, 0xD8, 0x2C, 0x80, 0x78, 0x0A, 0x00, 0x00, 0x49, 0xE4, 0x28, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x49, 0xF0,
	    0x2C, 0x00, 0x68, 0x08, 0x00, 0x00, 0x49, 0xFC, 0x23, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x4A, 0x04, 0x2C, 0x80,
	    0x78, 0x08, 0x00, 0x00, 0x4A, 0x10, 0x23, 0x1B, 0x80, 0x13, 0x00, 0x00, 0x5C, 0x98, 0x1E, 0x0C, 0x80, 0x80,
	    0x00, 0x00, 0x5C, 0xF4, 0xB4, 0x80, 0x0C, 0x90, 0x00, 0x00, 0x5D, 0x08, 0xB4, 0x80, 0x0C, 0x90, 0x00, 0x00,
	    0x00, 0xFF, 0x00, 0x00, 0x3A, 0x1C, 0xB4, 0x94, 0x00, 0x13, 0x00, 0x00, 0x3A, 0x64, 0x2C, 0x00, 0x00, 0x15,
	    0x00, 0x00, 0x3A, 0x70, 0xB4, 0x92, 0x80, 0x13, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x64, 0x7C, 0xB4, 0x9A, 0x40, 0x17, 0x00, 0x00, 0x64, 0x80,
	    0x64, 0x00, 0x10, 0x97, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x33, 0xE4, 0x42, 0xDE,
	    0x00, 0x00, 0x00, 0x00, 0x45, 0x28, 0x2C, 0x01, 0x30, 0x11, 0x00, 0x00, 0x45, 0x34, 0xB4, 0x98, 0x80, 0x13,
	    0x00, 0x00, 0x45, 0x3C, 0x2C, 0x81, 0x30, 0x11, 0x00, 0x00, 0x45, 0x48, 0xB4, 0x98, 0x80, 0x13, 0x00, 0x00,
	    0x45, 0x50, 0x2D, 0x00, 0x20, 0x11, 0x00, 0x00, 0x45, 0x5C, 0xB4, 0x98, 0x80, 0x13, 0x00, 0x00, 0x45, 0xF8,
	    0x2C, 0x01, 0x30, 0x0F, 0x00, 0x00, 0x46, 0x08, 0x0F, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x46, 0x0C, 0x2C, 0x81,
	    0x28, 0x0F, 0x00, 0x00, 0x46, 0x1C, 0x0F, 0x00, 0x01, 0x0B, 0x00, 0x00, 0x4A, 0xEC, 0x2C, 0x00, 0x70, 0x03,
	    0x00, 0x00, 0x4B, 0x00, 0x2C, 0x80, 0x38, 0x03, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x48, 0x5C, 0x2C, 0x00, 0x00, 0x0F, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x37, 0xB0,
	    0x3F, 0x59, 0x99, 0x9A, 0x00, 0x00, 0x37, 0xCC, 0x42, 0xAA, 0x00, 0x00, 0x00, 0x00, 0x55, 0x20, 0x87, 0x11,
	    0x80, 0x13, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x3B, 0x8C, 0x44, 0x0C, 0x00, 0x00,
	    0x00, 0x00, 0x3D, 0x0C, 0x44, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00,
	    0x50, 0xE4, 0xB4, 0x99, 0x00, 0x13, 0x00, 0x00, 0x50, 0xF8, 0xB4, 0x99, 0x00, 0x13, 0x00, 0x00, 0x00, 0xFF,
	    0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x4E, 0xB0, 0x02, 0xBC, 0xFF, 0x38, 0x00, 0x00,
	    0x4E, 0xBC, 0x14, 0x00, 0x01, 0x23, 0x00, 0x00, 0x4E, 0xC4, 0x03, 0x84, 0x01, 0xF4, 0x00, 0x00, 0x4E, 0xD0,
	    0x14, 0x00, 0x01, 0x23, 0x00, 0x00, 0x4E, 0xD8, 0x04, 0x4C, 0x04, 0xB0, 0x00, 0x00, 0x4E, 0xE4, 0x14, 0x00,
	    0x01, 0x23, 0x00, 0x00, 0x50, 0x5C, 0x2C, 0x00, 0x68, 0x15, 0x00, 0x00, 0x50, 0x6C, 0x14, 0x08, 0x01, 0x23,
	    0x00, 0x00, 0x50, 0x70, 0x2C, 0x80, 0x60, 0x15, 0x00, 0x00, 0x50, 0x80, 0x14, 0x08, 0x01, 0x23, 0x00, 0x00,
	    0x50, 0x84, 0x2D, 0x00, 0x20, 0x15, 0x00, 0x00, 0x50, 0x94, 0x14, 0x08, 0x01, 0x23, 0x00, 0x00, 0x00, 0xFF,
	    0x00, 0x00, 0x00, 0xFF, 0xBA, 0x81, 0x00, 0x08, 0x80, 0x01, 0x00, 0xB4, 0x38, 0x21, 0x00, 0xB0, 0x7C, 0x08,
	    0x03, 0xA6, 0x3C, 0x60, 0x80, 0x3C, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x2F, 0x9A, 0x3C,
	    0x00, 0x00, 0x00, 0x08, // #Common/PAL/Handlers/PAL Stock Icons.asm
	    0x88, 0x62, 0xF2, 0x34, 0x2C, 0x03, 0x00, 0x00, 0x41, 0x82, 0x00, 0x30, 0x48, 0x00, 0x00, 0x21, 0x7C, 0x88,
	    0x02, 0xA6, 0x80, 0x64, 0x00, 0x00, 0x90, 0x7D, 0x00, 0x2C, 0x90, 0x7D, 0x00, 0x30, 0x80, 0x64, 0x00, 0x04,
	    0x90, 0x7D, 0x00, 0x3C, 0x48, 0x00, 0x00, 0x10, 0x4E, 0x80, 0x00, 0x21, 0x3F, 0x59, 0x99, 0x9A, 0xC1, 0xA8,
	    0x00, 0x00, 0x80, 0x1D, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x10, 0xFC, 0x44, 0x00, 0x00, 0x00,
	    0x04, // #Common/PAL/Handlers/DK
	          // Up B/Aerial Up B.asm
	    0x88, 0x82, 0xF2, 0x34, 0x2C, 0x04, 0x00, 0x00, 0x41, 0x82, 0x00, 0x10, 0x3C, 0x00, 0x80, 0x11, 0x60, 0x00,
	    0x00, 0x74, 0x48, 0x00, 0x00, 0x08, 0x38, 0x03, 0xD7, 0x74, 0x00, 0x00, 0x00, 0x00, 0xC2, 0x10, 0xFB, 0x64,
	    0x00, 0x00, 0x00, 0x04, // #Common/PAL/Handlers/DK Up B/Grounded
	                            // Up B.asm
	    0x88, 0x82, 0xF2, 0x34, 0x2C, 0x04, 0x00, 0x00, 0x41, 0x82, 0x00, 0x10, 0x3C, 0x00, 0x80, 0x11, 0x60, 0x00,
	    0x00, 0x74, 0x48, 0x00, 0x00, 0x08, 0x38, 0x03, 0xD7, 0x74, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00 // Termination sequence
	};

	static std::unordered_map<u32, bool> staticBlacklist = {
	    {0x8008d698, true}, // Recording/GetLCancelStatus/GetLCancelStatus.asm
	    {0x8006c324, true}, // Recording/GetLCancelStatus/ResetLCancelStatus.asm
	    {0x800679bc, true}, // Recording/ExtendPlayerBlock.asm
	    {0x802fef88, true}, // Recording/FlushFrameBuffer.asm
	    {0x80005604, true}, // Recording/IsVSMode.asm
	    {0x8016d30c, true}, // Recording/SendGameEnd.asm
	    {0x8016e74c, true}, // Recording/SendGameInfo.asm
	    {0x8006c5d8, true}, // Recording/SendGamePostFrame.asm
	    {0x8006b0dc, true}, // Recording/SendGamePreFrame.asm
	    {0x803219ec, true}, // 3.4.0: Recording/FlushFrameBuffer.asm (Have to keep old ones for backward compatibility)
	    {0x8006da34, true}, // 3.4.0: Recording/SendGamePostFrame.asm

	    {0x8021aae4, true}, // Binary/FasterMeleeSettings/DisableFdTransitions.bin
	    {0x801cbb90, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x801CC8AC, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x801CBE9C, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x801CBEF0, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x801CBF54, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x80390838, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x801CD250, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x801CCDCC, true}, // Binary/FasterMeleeSettings/LaglessFod.bin
	    {0x801C26B0, true}, // Binary/FasterMeleeSettings/RandomStageMusic.bin
	    {0x803761ec, true}, // Binary/NormalLagReduction.bin
	    {0x800198a4, true}, // Binary/PerformanceLagReduction.bin
	    {0x80019620, true}, // Binary/PerformanceLagReduction.bin
	    {0x801A5054, true}, // Binary/PerformanceLagReduction.bin
	    {0x80023ffc, true}, // Binary/GameMusicOn.bin + Binary/GameMusicOff.bin
	    {0x80397878, true}, // Binary/OsReportPrintOnCrash.bin
	    {0x801A4DA0, true}, // Binary/LagReduction/PD.bin
	    {0x801A4DB4, true}, // Binary/LagReduction/PD.bin
	    {0x80019860, true}, // Binary/LagReduction/PD.bin
	    {0x801A4C24, true}, // Binary/LagReduction/PD+VB.bin
	    {0x8001985C, true}, // Binary/LagReduction/PD+VB.bin
	    {0x80019860, true}, // Binary/LagReduction/PD+VB.bin
	    {0x80376200, true}, // Binary/LagReduction/PD+VB.bin
	    {0x801A5018, true}, // Binary/LagReduction/PD+VB.bin
	    {0x80218D68, true}, // Binary/LagReduction/PD+VB.bin

	    {0x800055f0, true}, // Common/EXITransferBuffer.asm
	    {0x800055f8, true}, // Common/GetIsFollower.asm
	    {0x800055fc, true}, // Common/Gecko/ProcessCodeList.asm
	    {0x8016d294, true}, // Common/IncrementFrameIndex.asm

	    {0x801a5b14, true}, // External/Salty Runback/Salty Runback.asm
	    {0x801a4570, true}, // External/LagReduction/ForceHD/480pDeflickerOff.asm
	    {0x802fccd8, true}, // External/Hide Nametag When Invisible/Hide Nametag When Invisible.asm

	    {0x804ddb30,
	     true}, // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble Positions/Adjust Corner Value 1.asm
	    {0x804ddb34,
	     true}, // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble Positions/Adjust Corner Value 2.asm
	    {0x804ddb2c, true}, // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble Positions/Extend Negative
	                        // Vertical Bound.asm
	    {0x804ddb28, true}, // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble Positions/Extend Positive
	                        // Vertical Bound.asm
	    {0x804ddb4c, true}, // External/Widescreen/Adjust Offscreen Scissor/Fix Bubble Positions/Widen Bubble Region.asm
	    {0x804ddb58, true}, // External/Widescreen/Adjust Offscreen Scissor/Adjust Bubble Zoom.asm
	    {0x80086b24, true}, // External/Widescreen/Adjust Offscreen Scissor/Draw High Poly Models.asm
	    {0x80030C7C, true}, // External/Widescreen/Adjust Offscreen Scissor/Left Camera Bound.asm
	    {0x80030C88, true}, // External/Widescreen/Adjust Offscreen Scissor/Right Camera Bound.asm
	    {0x802fcfc4, true}, // External/Widescreen/Nametag Fixes/Adjust Nametag Background X Scale.asm
	    {0x804ddb84, true}, // External/Widescreen/Nametag Fixes/Adjust Nametag Text X Scale.asm
	    {0x803BB05C, true}, // External/Widescreen/Fix Screen Flash.asm
	    {0x8036A4A8, true}, // External/Widescreen/Overwrite CObj Values.asm

	    {0x801a4de4, true}, // Start engine loop
	    {0x8038d0b0, true}, // PreventDuplicateSounds
	    {0x801a5014, true}, // LoopEngineForRollback
	};

	std::unordered_map<u32, bool> blacklist;
	blacklist.insert(staticBlacklist.begin(), staticBlacklist.end());

	auto replayCommSettings = replayComm->getSettings();
	if (replayCommSettings.rollbackDisplayMethod == "off")
	{
		// Some codes should only be blacklisted when not displaying rollbacks, these are codes
		// that are required for things to not break when using Slippi savestates. Perhaps this
		// should be handled by actually applying these codes in the playback ASM instead? not sure
		blacklist[0x8038add0] = true; // PreventMusicAlarm
	}

	geckoList.clear();

	Slippi::GameSettings *settings = m_current_game->GetSettings();
	if (settings->geckoCodes.empty())
	{
		geckoList = defaultCodeList;
		return;
	}

	std::vector<u8> source = settings->geckoCodes;
	INFO_LOG(SLIPPI, "Booting codes with source size: %d", source.size());

	int idx = 0;
	while (idx < source.size())
	{
		u8 codeType = source[idx] & 0xFE;
		u32 address = source[idx] << 24 | source[idx + 1] << 16 | source[idx + 2] << 8 | source[idx + 3];
		address = (address & 0x01FFFFFF) | 0x80000000;

		u32 codeOffset = 8; // Default code offset. Most codes are this length
		switch (codeType)
		{
		case 0xC0:
		case 0xC2:
		{
			u32 lineCount = source[idx + 4] << 24 | source[idx + 5] << 16 | source[idx + 6] << 8 | source[idx + 7];
			codeOffset = 8 + (lineCount * 8);
			break;
		}
		case 0x08:
			codeOffset = 16;
			break;
		case 0x06:
		{
			u32 byteLen = source[idx + 4] << 24 | source[idx + 5] << 16 | source[idx + 6] << 8 | source[idx + 7];
			codeOffset = 8 + ((byteLen + 7) & 0xFFFFFFF8); // Round up to next 8 bytes and add the first 8 bytes
			break;
		}
		}

		idx += codeOffset;

		// If this address is blacklisted, we don't add it to what we will send to game
		if (blacklist.count(address))
			continue;

		INFO_LOG(SLIPPI, "Codetype [%x] Inserting section: %d - %d (%x, %d)", codeType, idx - codeOffset, idx, address,
		         codeOffset);

		// If not blacklisted, add code to return vector
		geckoList.insert(geckoList.end(), source.begin() + (idx - codeOffset), source.begin() + idx);
	}

	// Add the termination sequence
	geckoList.insert(geckoList.end(), {0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
}

void CEXISlippi::prepareCharacterFrameData(Slippi::FrameData *frame, u8 port, u8 isFollower)
{
	std::unordered_map<uint8_t, Slippi::PlayerFrameData> source;
	source = isFollower ? frame->followers : frame->players;

	// This must be updated if new data is added
	int characterDataLen = 49;

	// Check if player exists
	if (!source.count(port))
	{
		// If player does not exist, insert blank section
		m_read_queue.insert(m_read_queue.end(), characterDataLen, 0);
		return;
	}

	// Get data for this player
	Slippi::PlayerFrameData data = source[port];

	// log << frameIndex << "\t" << port << "\t" << data.locationX << "\t" << data.locationY << "\t" <<
	// data.animation
	// << "\n";

	// WARN_LOG(EXPANSIONINTERFACE, "[Frame %d] [Player %d] Positions: %f | %f", frameIndex, port, data.locationX,
	// data.locationY);

	// Add all of the inputs in order
	appendWordToBuffer(&m_read_queue, data.randomSeed);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.joystickX);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.joystickY);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.cstickX);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.cstickY);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.trigger);
	appendWordToBuffer(&m_read_queue, data.buttons);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.locationX);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.locationY);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.facingDirection);
	appendWordToBuffer(&m_read_queue, (u32)data.animation);
	m_read_queue.push_back(data.joystickXRaw);
	appendWordToBuffer(&m_read_queue, *(u32 *)&data.percent);
	// NOTE TO DEV: If you add data here, make sure to increase the size above
}

bool CEXISlippi::checkFrameFullyFetched(int32_t frameIndex)
{
	auto doesFrameExist = m_current_game->DoesFrameExist(frameIndex);
	if (!doesFrameExist)
		return false;

	Slippi::FrameData *frame = m_current_game->GetFrame(frameIndex);

	// This flag is set to true after a post frame update has been received. At that point
	// we know we have received all of the input data for the frame
	return frame->inputsFullyFetched;
}

void CEXISlippi::prepareFrameData(u8 *payload)
{
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game)
	{
		// Do nothing if we don't have a game loaded
		return;
	}

	// Parse input
	int32_t frameIndex = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

	// If loading from queue, move on to the next replay if we have past endFrame
	auto watchSettings = replayComm->current;
	if (frameIndex > watchSettings.endFrame)
	{
		INFO_LOG(SLIPPI, "Killing game because we are past endFrame");
		m_read_queue.push_back(FRAME_RESP_TERMINATE);
		return;
	}

	// If a new replay should be played, terminate the current game
	auto isNewReplay = replayComm->isNewReplay();
	if (isNewReplay)
	{
		m_read_queue.push_back(FRAME_RESP_TERMINATE);
		return;
	}

	auto isProcessingComplete = m_current_game->IsProcessingComplete();
	// Wait until frame exists in our data before reading it. We also wait until
	// next frame has been found to ensure we have actually received all of the
	// data from this frame. Don't wait until next frame is processing is complete
	// (this is the last frame, in that case)
	auto isFrameFound = m_current_game->DoesFrameExist(frameIndex);
	g_playback_status->latestFrame = m_current_game->GetLatestIndex();
	auto isNextFrameFound = g_playback_status->latestFrame > frameIndex;
	auto isFrameComplete = checkFrameFullyFetched(frameIndex);
	auto isFrameReady = isFrameFound && (isProcessingComplete || isNextFrameFound || isFrameComplete);

	// If there is a startFrame configured, manage the fast-forward flag
	if (watchSettings.startFrame > Slippi::GAME_FIRST_FRAME)
	{
		if (frameIndex < watchSettings.startFrame)
		{
			isHardFFW = true;
		}
		else if (frameIndex == watchSettings.startFrame)
		{
			// TODO: This might disable fast forward on first frame when we dont want to?
			isHardFFW = false;
		}
	}

	auto commSettings = replayComm->getSettings();
	if (commSettings.rollbackDisplayMethod == "normal")
	{
		auto nextFrame = m_current_game->GetFrameAt(frameSeqIdx);
		isHardFFW = nextFrame && nextFrame->frame <= g_playback_status->currentPlaybackFrame;

		if (nextFrame)
		{
			// This feels jank but without this g_playback_status ends up getting updated to
			// a value beyond the frame that actually gets played causes too much FFW
			frameIndex = nextFrame->frame;
		}
	}

	// If RealTimeMode is enabled, let's trigger fast forwarding under certain conditions
	auto isFarBehind = g_playback_status->latestFrame - frameIndex > 2;
	auto isVeryFarBehind = g_playback_status->latestFrame - frameIndex > 25;
	if (isFarBehind && commSettings.mode == "mirror" && commSettings.isRealTimeMode)
	{
		isSoftFFW = true;

		// Once isHardFFW has been turned on, do not turn it off with this condition, should
		// hard FFW to the latest point
		if (!isHardFFW)
			isHardFFW = isVeryFarBehind;
	}

	if (g_playback_status->latestFrame == frameIndex)
	{
		// The reason to disable fast forwarding here is in hopes
		// of disabling it on the last frame that we have actually received.
		// Doing this will allow the rendering logic to run to display the
		// last frame instead of the frame previous to fast forwarding.
		// Not sure if this fully works with partial frames
		isSoftFFW = false;
		isHardFFW = false;
	}

	bool shouldFFW = shouldFFWFrame(frameIndex);
	u8 requestResultCode = shouldFFW ? FRAME_RESP_FASTFORWARD : FRAME_RESP_CONTINUE;
	if (!isFrameReady)
	{
		// If processing is complete, the game has terminated early. Tell our playback
		// to end the game as well.
		auto shouldTerminateGame = isProcessingComplete;
		requestResultCode = shouldTerminateGame ? FRAME_RESP_TERMINATE : FRAME_RESP_WAIT;
		m_read_queue.push_back(requestResultCode);

		// Disable fast forward here too... this shouldn't be necessary but better
		// safe than sorry I guess
		isSoftFFW = false;
		isHardFFW = false;

		if (requestResultCode == FRAME_RESP_TERMINATE)
		{
			ERROR_LOG(EXPANSIONINTERFACE, "Game should terminate on frame %d [%X]", frameIndex, frameIndex);
		}

		return;
	}

	u8 rollbackCode = 0; // 0 = not rollback, 1 = rollback, perhaps other options in the future?

	// Increment frame index if greater
	if (frameIndex > g_playback_status->currentPlaybackFrame)
	{
		g_playback_status->currentPlaybackFrame = frameIndex;
	}
	else if (commSettings.rollbackDisplayMethod != "off")
	{
		rollbackCode = 1;
	}

	// WARN_LOG(EXPANSIONINTERFACE, "[Frame %d] Playback current behind by: %d frames.", frameIndex,
	//        g_playback_status->latestFrame - frameIndex);

	// Keep track of last FFW frame, used for soft FFW's
	if (shouldFFW)
	{
		WARN_LOG(EXPANSIONINTERFACE, "[Frame %d] FFW frame, behind by: %d frames.", frameIndex,
		         g_playback_status->latestFrame - frameIndex);
		lastFFWFrame = frameIndex;
	}

	// Return success code
	m_read_queue.push_back(requestResultCode);

	// Get frame
	Slippi::FrameData *frame = m_current_game->GetFrame(frameIndex);
	if (commSettings.rollbackDisplayMethod != "off")
	{
		auto previousFrame = m_current_game->GetFrameAt(frameSeqIdx - 1);
		frame = m_current_game->GetFrameAt(frameSeqIdx);

		*(s32 *)(&playbackSavestatePayload[0]) = Common::swap32(frame->frame);

		if (previousFrame && frame->frame <= previousFrame->frame)
		{
			// Here we should load a savestate
			handleLoadSavestate(&playbackSavestatePayload[0]);
		}

		// Here we should save a savestate
		handleCaptureSavestate(&playbackSavestatePayload[0]);

		frameSeqIdx += 1;
	}
	// else
	//{
	//	std::vector<u8> fakePayload(8, 0);
	//	*(s32 *)(&fakePayload[0]) = Common::swap32(frame->frame);

	//	if (frame->frame == 400)
	//	{
	//		handleCaptureSavestate(&fakePayload[0]);
	//	}

	//	if (frame->frame == 950)
	//	{
	//		*(s32 *)(&fakePayload[0]) = Common::swap32(400);
	//		handleLoadSavestate(&fakePayload[0]);
	//		handleCaptureSavestate(&fakePayload[0]);
	//	}
	//}

	// For normal replays, modify slippi seek/playback data as needed
	// TODO: maybe handle other modes too?
	if (commSettings.mode == "normal" || commSettings.mode == "queue")
	{
		prepareSlippiPlayback(frame->frame);
	}

	// Push RB code
	m_read_queue.push_back(rollbackCode);

	// Add frame rng seed to be restored at priority 0
	u8 rngResult = frame->randomSeedExists ? 1 : 0;
	m_read_queue.push_back(rngResult);
	appendWordToBuffer(&m_read_queue, *(u32 *)&frame->randomSeed);

	// Add frame data for every character
	for (u8 port = 0; port < 4; port++)
	{
		prepareCharacterFrameData(frame, port, 0);
		prepareCharacterFrameData(frame, port, 1);
	}
}

bool CEXISlippi::shouldFFWFrame(int32_t frameIndex)
{
	if (!isSoftFFW && !isHardFFW)
	{
		// If no FFW at all, don't FFW this frame
		return false;
	}

	if (isHardFFW)
	{
		// For a hard FFW, always FFW until it's turned off
		return true;
	}

	// Here we have a soft FFW, we only want to turn on FFW for single frames once
	// every X frames to FFW in a more smooth manner
	return frameIndex - lastFFWFrame >= 15;
}

void CEXISlippi::prepareIsStockSteal(u8 *payload)
{
	// Since we are prepping new data, clear any existing data
	m_read_queue.clear();

	if (!m_current_game)
	{
		// Do nothing if we don't have a game loaded
		return;
	}

	// Parse args
	int32_t frameIndex = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
	u8 playerIndex = payload[4];

	// I'm not sure checking for the frame should be necessary. Theoretically this
	// should get called after the frame request so the frame should already exist
	auto isFrameFound = m_current_game->DoesFrameExist(frameIndex);
	if (!isFrameFound)
	{
		m_read_queue.push_back(0);
		return;
	}

	// Load the data from this frame into the read buffer
	Slippi::FrameData *frame = m_current_game->GetFrame(frameIndex);
	auto players = frame->players;

	u8 playerIsBack = players.count(playerIndex) ? 1 : 0;
	m_read_queue.push_back(playerIsBack);
}

void CEXISlippi::prepareIsFileReady()
{
	m_read_queue.clear();

	auto isNewReplay = replayComm->isNewReplay();
	if (!isNewReplay)
	{
		replayComm->nextReplay();
		m_read_queue.push_back(0);
		return;
	}

	// Attempt to load game if there is a new replay file
	// this can come pack falsy if the replay file does not exist
	m_current_game = replayComm->loadGame();
	if (!m_current_game)
	{
		// Do not start if replay file doesn't exist
		// TODO: maybe display error message?
		INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Replay file does not exist?");
		m_read_queue.push_back(0);
		return;
	}

	INFO_LOG(SLIPPI, "EXI_DeviceSlippi.cpp: Replay file loaded successfully!?");

	// Clear playback control related vars
	resetPlayback();

	// Start the playback!
	m_read_queue.push_back(1);
}

static int tempTestCount = 0;
void CEXISlippi::handleOnlineInputs(u8 *payload)
{
	m_read_queue.clear();

	int32_t frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

	if (frame == 1)
	{
		availableSavestates.clear();
		activeSavestates.clear();

		// Prepare savestates for online play
		for (int i = 0; i < ROLLBACK_MAX_FRAMES; i++)
		{
			availableSavestates.push_back(std::make_unique<SlippiSavestate>());
		}

		// Reset stall counter
		isConnectionStalled = false;
		stallFrameCount = 0;

		// Reset character selections as they are no longer needed
		localSelections.Reset();
		slippi_netplay->StartSlippiGame();
	}

	if (shouldSkipOnlineFrame(frame))
	{
		// Send inputs that have not yet been acked
		slippi_netplay->SendSlippiPad(nullptr);
		m_read_queue.push_back(2);
		return;
	}

	handleSendInputs(payload);
	prepareOpponentInputs(payload);
}

bool CEXISlippi::shouldSkipOnlineFrame(int32_t frame)
{
	auto status = slippi_netplay->GetSlippiConnectStatus();
	bool connectionFailed = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_FAILED;
	bool connectionDisconnected = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_DISCONNECTED;
	if (connectionFailed || connectionDisconnected)
	{
		// If connection failed just continue the game
		return false;
	}

	if (isConnectionStalled)
	{
		return false;
	}

	// Return true if we are too far ahead for rollback. ROLLBACK_MAX_FRAMES is the number of frames
	// we can receive for the opponent at one time and is our "look-ahead" limit
	int32_t latestRemoteFrame = slippi_netplay->GetSlippiLatestRemoteFrame();
	if (frame - latestRemoteFrame >= ROLLBACK_MAX_FRAMES)
	{
		stallFrameCount++;
		if (stallFrameCount > 60 * 7)
		{
			// 7 second stall will disconnect game
			isConnectionStalled = true;
		}

		WARN_LOG(SLIPPI_ONLINE, "Halting for one frame due to rollback limit (frame: %d | latest: %d)...", frame,
		         latestRemoteFrame);
		return true;
	}

	stallFrameCount = 0;

	// Return true if we are over 60% of a frame ahead of our opponent. Currently limiting how
	// often this happens because I'm worried about jittery data causing a lot of unneccesary delays.
	// Only skip once for a given frame because our time detection method doesn't take into consideration
	// waiting for a frame. Also it's less jarring and it happens often enough that it will smoothly
	// get to the right place
	auto isTimeSyncFrame = frame % SLIPPI_ONLINE_LOCKSTEP_INTERVAL; // Only time sync every 30 frames
	if (isTimeSyncFrame == 0 && !isCurrentlySkipping)
	{
		auto offsetUs = slippi_netplay->CalcTimeOffsetUs();
		INFO_LOG(SLIPPI_ONLINE, "[Frame %d] Offset is: %d us", frame, offsetUs);

		if (offsetUs > 10000)
		{
			isCurrentlySkipping = true;

			int maxSkipFrames = frame <= 120 ? 5 : 1; // On early frames, support skipping more frames
			framesToSkip = ((offsetUs - 10000) / 16683) + 1;
			framesToSkip = framesToSkip > maxSkipFrames ? maxSkipFrames : framesToSkip; // Only skip 5 frames max

			WARN_LOG(SLIPPI_ONLINE, "Halting on frame %d due to time sync. Offset: %d us. Frames: %d...", frame,
			         offsetUs, framesToSkip);
		}
	}

	// Handle the skipped frames
	if (framesToSkip > 0)
	{
		// If ahead by 60% of a frame, stall. I opted to use 60% instead of half a frame
		// because I was worried about two systems continuously stalling for each other
		framesToSkip = framesToSkip - 1;
		return true;
	}

	isCurrentlySkipping = false;

	return false;
}

void CEXISlippi::handleSendInputs(u8 *payload)
{
	if (isConnectionStalled)
		return;

	int32_t frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
	u8 delay = payload[4];

	auto pad = std::make_unique<SlippiPad>(frame + delay, &payload[5]);

	slippi_netplay->SendSlippiPad(std::move(pad));
}

void CEXISlippi::prepareOpponentInputs(u8 *payload)
{
	m_read_queue.clear();

	u8 frameResult = 1; // Indicates to continue frame

	auto state = slippi_netplay->GetSlippiConnectStatus();
	if (state != SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED || isConnectionStalled)
	{
		frameResult = 3; // Indicates we have disconnected
	}

	m_read_queue.push_back(frameResult); // Indicate a continue frame

	int32_t frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

	auto result = slippi_netplay->GetSlippiRemotePad(frame);

	// determine offset from which to copy data
	int offset = (result->latestFrame - frame) * SLIPPI_PAD_FULL_SIZE;
	offset = offset < 0 ? 0 : offset;

	// add latest frame we are transfering to begining of return buf
	int32_t latestFrame = offset > 0 ? frame : result->latestFrame;
	appendWordToBuffer(&m_read_queue, *(u32 *)&latestFrame);

	// copy pad data over
	auto txStart = result->data.begin() + offset;
	auto txEnd = result->data.end();

	std::vector<u8> tx;
	tx.insert(tx.end(), txStart, txEnd);
	tx.resize(SLIPPI_PAD_FULL_SIZE * ROLLBACK_MAX_FRAMES, 0);

	m_read_queue.insert(m_read_queue.end(), tx.begin(), tx.end());

	// ERROR_LOG(SLIPPI_ONLINE, "EXI: [%d] %X %X %X %X %X %X %X %X", latestFrame, m_read_queue[5], m_read_queue[6],
	// m_read_queue[7], m_read_queue[8], m_read_queue[9], m_read_queue[10], m_read_queue[11], m_read_queue[12]);
}

void CEXISlippi::handleCaptureSavestate(u8 *payload)
{
	s32 frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];

	u64 startTime = Common::Timer::GetTimeUs();

	// Grab an available savestate
	std::unique_ptr<SlippiSavestate> ss;
	if (!availableSavestates.empty())
	{
		ss = std::move(availableSavestates.back());
		availableSavestates.pop_back();
	}
	else
	{
		// If there were no available savestates, use the oldest one
		auto it = activeSavestates.begin();
		ss = std::move(it->second);
		activeSavestates.erase(it->first);
	}

	// If there is already a savestate for this frame, remove it and add it to available
	if (activeSavestates.count(frame))
	{
		availableSavestates.push_back(std::move(activeSavestates[frame]));
		activeSavestates.erase(frame);
	}

	ss->Capture();
	activeSavestates[frame] = std::move(ss);

	u32 timeDiff = (u32)(Common::Timer::GetTimeUs() - startTime);
	INFO_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Captured savestate for frame %d in: %f ms", frame,
	         ((double)timeDiff) / 1000);
}

void CEXISlippi::handleLoadSavestate(u8 *payload)
{
	s32 frame = payload[0] << 24 | payload[1] << 16 | payload[2] << 8 | payload[3];
	u32 *preserveArr = (u32 *)(&payload[4]);

	if (!activeSavestates.count(frame))
	{
		// This savestate does not exist... uhhh? What do we do?
		ERROR_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Savestate for frame %d does not exist.", frame);
		return;
	}

	u64 startTime = Common::Timer::GetTimeUs();

	// Fetch preservation blocks
	std::vector<SlippiSavestate::PreserveBlock> blocks;

	// Get preservation blocks
	int idx = 0;
	while (Common::swap32(preserveArr[idx]) != 0)
	{
		SlippiSavestate::PreserveBlock p = {Common::swap32(preserveArr[idx]), Common::swap32(preserveArr[idx + 1])};
		blocks.push_back(p);
		idx += 2;
	}

	// Load savestate
	activeSavestates[frame]->Load(blocks);

	// Move all active savestates to available
	for (auto it = activeSavestates.begin(); it != activeSavestates.end(); ++it)
	{
		availableSavestates.push_back(std::move(it->second));
	}

	activeSavestates.clear();

	u32 timeDiff = (u32)(Common::Timer::GetTimeUs() - startTime);
	INFO_LOG(SLIPPI_ONLINE, "SLIPPI ONLINE: Loaded savestate for frame %d in: %f ms", frame, ((double)timeDiff) / 1000);
}

void CEXISlippi::startFindMatch(u8 *payload)
{
#ifndef LOCAL_TESTING
	SlippiMatchmaking::MatchSearchSettings search;
	search.mode = (SlippiMatchmaking::OnlinePlayMode)payload[0];

	std::string shiftJisCode;
	shiftJisCode.insert(shiftJisCode.begin(), &payload[1], &payload[1] + 18);
	shiftJisCode.erase(std::find(shiftJisCode.begin(), shiftJisCode.end(), 0x00), shiftJisCode.end());

	// TODO: Make this work so we dont have to pass shiftJis to mm server
	// search.connectCode = SHIFTJISToUTF8(shiftJisCode).c_str();
	search.connectCode = shiftJisCode;

	matchmaking->FindMatch(search);
#endif
}

void CEXISlippi::prepareOnlineMatchState()
{
	// This match block is a VS match with P1 Red Falco vs P2 Red Bowser on Battlefield. The proper values will
	// be overwritten
	static std::vector<u8> onlineMatchBlock = {
	    0x32, 0x01, 0x86, 0x4C, 0xC3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x6E, 0x00, 0x1F, 0x00, 0x00,
	    0x01, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
	    0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x05, 0x00, 0x04, 0x01, 0x00, 0x01, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0xC0, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x1A, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0x40, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x1A, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0x40, 0x00, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x21, 0x03, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x09, 0x00, 0x78, 0x00,
	    0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3F, 0x80, 0x00, 0x00, 0x3F, 0x80,
	    0x00, 0x00, 0x3F, 0x80, 0x00, 0x00,
	};

	m_read_queue.clear();

	SlippiMatchmaking::ProcessState mmState = matchmaking->GetMatchmakeState();

#ifdef LOCAL_TESTING
	if (localSelections.isCharacterSelected)
		mmState = SlippiMatchmaking::ProcessState::CONNECTION_SUCCESS;
#endif

	m_read_queue.push_back(mmState); // Matchmaking State

	u8 localPlayerReady = localSelections.isCharacterSelected;
	u8 remotePlayerReady = 0;
	u8 localPlayerIndex = 0;
	u8 remotePlayerIndex = 1;

	std::string oppName = "";

	if (mmState == SlippiMatchmaking::ProcessState::CONNECTION_SUCCESS)
	{
		if (!slippi_netplay)
		{
#ifdef LOCAL_TESTING
			slippi_netplay = std::make_unique<SlippiNetplayClient>(true);
#else
			slippi_netplay = matchmaking->GetNetplayClient();
#endif

			slippi_netplay->SetMatchSelections(localSelections);
		}

#ifdef LOCAL_TESTING
		bool isConnected = true;
#else
		auto status = slippi_netplay->GetSlippiConnectStatus();
		bool isConnected = status == SlippiNetplayClient::SlippiConnectStatus::NET_CONNECT_STATUS_CONNECTED;
#endif

		if (isConnected)
		{
			auto matchInfo = slippi_netplay->GetMatchInfo();
#ifdef LOCAL_TESTING
			remotePlayerReady = true;
#else
			remotePlayerReady = matchInfo->remotePlayerSelections.isCharacterSelected;
#endif

			auto isDecider = slippi_netplay->IsDecider();
			localPlayerIndex = isDecider ? 0 : 1;
			remotePlayerIndex = isDecider ? 1 : 0;

			oppName = slippi_netplay->GetOpponentName();
		}
		else
		{
#ifndef LOCAL_TESTING
			// If we get here, our opponent likely disconnected. Let's trigger a clean up
			handleConnectionCleanup();
#endif
		}
	}
	else
	{
		slippi_netplay = nullptr;
	}

	m_read_queue.push_back(localPlayerReady);  // Local player ready
	m_read_queue.push_back(remotePlayerReady); // Remote player ready
	m_read_queue.push_back(localPlayerIndex);  // Local player index
	m_read_queue.push_back(remotePlayerIndex); // Remote player index

	u32 rngOffset = 0;
	std::string p1Name = "";
	std::string p2Name = "";

	if (localPlayerReady && remotePlayerReady)
	{
		auto isDecider = slippi_netplay->IsDecider();

		auto matchInfo = slippi_netplay->GetMatchInfo();
		SlippiPlayerSelections lps = matchInfo->localPlayerSelections;
		SlippiPlayerSelections rps = matchInfo->remotePlayerSelections;

		// Overwrite local player character
		onlineMatchBlock[0x60 + localPlayerIndex * 0x24] = lps.characterId;
		onlineMatchBlock[0x63 + localPlayerIndex * 0x24] = lps.characterColor;

#ifdef LOCAL_TESTING
		rps.characterId = 2;
		rps.characterColor = 2;
		rps.playerName = std::string("Player");
#endif

		// Overwrite remote player character
		onlineMatchBlock[0x60 + remotePlayerIndex * 0x24] = rps.characterId;
		onlineMatchBlock[0x63 + remotePlayerIndex * 0x24] = rps.characterColor;

		// Make one character lighter if same character, same color
		bool isSheikVsZelda =
		    lps.characterId == 0x12 && rps.characterId == 0x13 || lps.characterId == 0x13 && rps.characterId == 0x12;
		bool charMatch = lps.characterId == rps.characterId || isSheikVsZelda;
		bool colMatch = lps.characterColor == rps.characterColor;

		onlineMatchBlock[0x67 + 0x24] = charMatch && colMatch ? 1 : 0;

		// Overwrite stage
		u16 stageId;
		if (isDecider)
		{
			stageId = lps.isStageSelected ? lps.stageId : rps.stageId;
		}
		else
		{
			stageId = rps.isStageSelected ? rps.stageId : lps.stageId;
		}

		// int seconds = 0;
		// u32 *timer = (u32 *)&onlineMatchBlock[0x10];
		//*timer = Common::swap32(seconds * 60);

		u16 *stage = (u16 *)&onlineMatchBlock[0xE];
		*stage = Common::swap16(stageId);

		// Set rng offset
		rngOffset = isDecider ? lps.rngOffset : rps.rngOffset;
		WARN_LOG(SLIPPI_ONLINE, "Rng Offset: 0x%x", rngOffset);
		WARN_LOG(SLIPPI_ONLINE, "P1 Char: 0x%X, P2 Char: 0x%X", onlineMatchBlock[0x60], onlineMatchBlock[0x84]);

		// Set player names
		p1Name = isDecider ? lps.playerName : rps.playerName;
		p2Name = isDecider ? rps.playerName : lps.playerName;
	}

	// Add rng offset to output
	appendWordToBuffer(&m_read_queue, rngOffset);

	// Add delay frames to output
	m_read_queue.push_back((u8)SConfig::GetInstance().m_slippiOnlineDelay);

	// Add names to output
	p1Name = ConvertStringForGame(p1Name, MAX_NAME_LENGTH);
	m_read_queue.insert(m_read_queue.end(), p1Name.begin(), p1Name.end());
	p2Name = ConvertStringForGame(p2Name, MAX_NAME_LENGTH);
	m_read_queue.insert(m_read_queue.end(), p2Name.begin(), p2Name.end());
	oppName = ConvertStringForGame(oppName, MAX_NAME_LENGTH);
	m_read_queue.insert(m_read_queue.end(), oppName.begin(), oppName.end());

	// Add error message if there is one
	auto errorStr = matchmaking->GetErrorMessage();
	errorStr = ConvertStringForGame(errorStr, 120);
	m_read_queue.insert(m_read_queue.end(), errorStr.begin(), errorStr.end());

	// Add the match struct block to output
	m_read_queue.insert(m_read_queue.end(), onlineMatchBlock.begin(), onlineMatchBlock.end());
}

u16 CEXISlippi::getRandomStage()
{
	static u16 selectedStage;

	static std::vector<u16> stages = {
	    0x2,  // FoD
	          // 0x3, // Pokemon
	    0x8,  // Yoshi's Story
	    0x1C, // Dream Land
	    0x1F, // Battlefield
	    0x20, // Final Destination
	};

	std::vector<u16> stagesToConsider;

	// stagesToConsider = stages;
	// Add all stages to consider to the vector
	for (auto it = stages.begin(); it != stages.end(); ++it)
	{
		auto stageId = *it;
		if (lastSelectedStage != nullptr && stageId == *lastSelectedStage)
			continue;

		stagesToConsider.push_back(stageId);
	}

	// Shuffle the stages to consider. This isn't really necessary considering we
	// use a random number to select an index but idk the generator was giving a lot
	// of the same stage (same index) many times in a row or so it seemed to I figured
	// this can't hurt
	std::random_shuffle(stagesToConsider.begin(), stagesToConsider.end());

	// Get random stage
	int randIndex = generator() % stagesToConsider.size();
	selectedStage = stagesToConsider[randIndex];

	// Set last selected stage
	lastSelectedStage = &selectedStage;

	return selectedStage;
}

void CEXISlippi::setMatchSelections(u8 *payload)
{
	SlippiPlayerSelections s;

	s.characterId = payload[0];
	s.characterColor = payload[1];
	s.isCharacterSelected = payload[2];

	s.stageId = Common::swap16(&payload[3]);
	s.isStageSelected = payload[5];

	if (!s.isStageSelected)
	{
		// If stage is not selected, select a random stage
		s.stageId = getRandomStage();
		s.isStageSelected = true;
	}

	s.rngOffset = generator() % 0xFFFF;

	// Get user name from file
	std::string displayName = user->GetUserInfo().displayName;

	// Just let the max length to transfer to opponent be potentially 16 worst-case utf-8 chars
	// This string will get converted to the game format later
	int maxLenth = MAX_NAME_LENGTH * 4 + 4;
	if (displayName.length() > maxLenth)
	{
		displayName.resize(maxLenth);
	}

	s.playerName = displayName;

	// Get user connect code from file
	s.connectCode = user->GetUserInfo().connectCode;

	// Merge these selections
	localSelections.Merge(s);

	if (slippi_netplay)
	{
		slippi_netplay->SetMatchSelections(localSelections);
	}
}

void CEXISlippi::prepareFileLength(u8 *payload)
{
	m_read_queue.clear();

	std::string fileName((char *)&payload[0]);

	std::string contents;
	u32 size = gameFileLoader->LoadFile(fileName, contents);

	INFO_LOG(SLIPPI, "Getting file size for: %s -> %d", fileName.c_str(), size);

	// Write size to output
	appendWordToBuffer(&m_read_queue, size);
}

void CEXISlippi::prepareFileLoad(u8 *payload)
{
	m_read_queue.clear();

	std::string fileName((char *)&payload[0]);

	std::string contents;
	u32 size = gameFileLoader->LoadFile(fileName, contents);
	std::vector<u8> buf(contents.begin(), contents.end());

	INFO_LOG(SLIPPI, "Writing file contents: %s -> %d", fileName.c_str(), size);

	// Write the contents to output
	m_read_queue.insert(m_read_queue.end(), buf.begin(), buf.end());
}

void CEXISlippi::logMessageFromGame(u8 *payload)
{
	if (payload[0] == 0)
	{
		// The first byte indicates whether to log the time or not
		GENERIC_LOG(LogTypes::SLIPPI, (LogTypes::LOG_LEVELS)payload[1], "%s", (char *)&payload[2]);
	}
	else
	{
		GENERIC_LOG(LogTypes::SLIPPI, (LogTypes::LOG_LEVELS)payload[1], "%s: %llu", (char *)&payload[2],
		            Common::Timer::GetTimeUs());
	}
}

void CEXISlippi::handleLogInRequest()
{
	bool logInRes = user->AttemptLogin();
	if (!logInRes)
	{
//#ifndef LINUX_LOCAL_DEV
		main_frame->LowerRenderWindow();
//#endif
		user->OpenLogInPage();
		user->ListenForLogIn();
	}
}

void CEXISlippi::handleLogOutRequest()
{
	user->LogOut();
}

void CEXISlippi::handleUpdateAppRequest()
{
#ifdef _WIN32
	main_frame->LowerRenderWindow();
	user->UpdateApp();
	main_frame->DoExit();
#else
	CriticalAlertT("Automatic updates are not available for macOS and Linux, please update manually.");
#endif
}

void CEXISlippi::prepareOnlineStatus()
{
	m_read_queue.clear();

	auto isLoggedIn = user->IsLoggedIn();
	auto userInfo = user->GetUserInfo();

	u8 appState = 0;
	if (isLoggedIn)
	{
		// Check if we have the latest version, and if not, indicate we need to update
		version::Semver200_version latestVersion(userInfo.latestVersion);
		version::Semver200_version currentVersion(scm_slippi_semver_str);

		appState = latestVersion > currentVersion ? 2 : 1;
	}

	m_read_queue.push_back(appState);

	// Write player name (31 bytes)
	std::string playerName = ConvertStringForGame(userInfo.displayName, MAX_NAME_LENGTH);
	m_read_queue.insert(m_read_queue.end(), playerName.begin(), playerName.end());

	// Write connect code (10 bytes)
	std::string connectCode = userInfo.connectCode;
	char shiftJisHashtag[] = {(char)0x81, (char)0x94, (char)0x00};
	connectCode.resize(CONNECT_CODE_LENGTH);
	connectCode = ReplaceAll(connectCode, "#", shiftJisHashtag);
	auto codeBuf = connectCode.c_str();
	m_read_queue.insert(m_read_queue.end(), codeBuf, codeBuf + CONNECT_CODE_LENGTH + 2);
}

void doConnectionCleanup(std::unique_ptr<SlippiMatchmaking> mm, std::unique_ptr<SlippiNetplayClient> nc)
{
	if (mm)
		mm.reset();

	if (nc)
		nc.reset();
}

void CEXISlippi::handleConnectionCleanup()
{
	ERROR_LOG(SLIPPI_ONLINE, "Connection cleanup started...");

	// Handle destructors in a separate thread to not block the main thread
	std::thread cleanup(doConnectionCleanup, std::move(matchmaking), std::move(slippi_netplay));
	cleanup.detach();

	// Reset matchmaking
	matchmaking = std::make_unique<SlippiMatchmaking>(user.get());

	// Disconnect netplay client
	slippi_netplay = nullptr;

	// Clear character selections
	localSelections.Reset();

	ERROR_LOG(SLIPPI_ONLINE, "Connection cleanup completed...");
}

void CEXISlippi::DMAWrite(u32 _uAddr, u32 _uSize)
{
	u8 *memPtr = Memory::GetPointer(_uAddr);

	u32 bufLoc = 0;

	if (memPtr == nullptr)
	{
		NOTICE_LOG(SLIPPI, "DMA Write was passed an invalid address: %x", _uAddr);
		Dolphin_Debugger::PrintCallstack(LogTypes::LOG_TYPE::SLIPPI, LogTypes::LOG_LEVELS::LNOTICE);
		m_read_queue.clear();
		return;
	}

	u8 byte = memPtr[0];
	if (byte == CMD_RECEIVE_COMMANDS)
	{
		time(&gameStartTime); // Store game start time
		u8 receiveCommandsLen = memPtr[1];
		configureCommands(&memPtr[1], receiveCommandsLen);
		writeToFileAsync(&memPtr[0], receiveCommandsLen + 1, "create");
		bufLoc += receiveCommandsLen + 1;
	}

	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI DMAWrite: addr: 0x%08x size: %d, bufLoc:[%02x %02x %02x %02x %02x]",
	         _uAddr, _uSize, memPtr[bufLoc], memPtr[bufLoc + 1], memPtr[bufLoc + 2], memPtr[bufLoc + 3],
	         memPtr[bufLoc + 4]);

	while (bufLoc < _uSize)
	{
		byte = memPtr[bufLoc];
		if (!payloadSizes.count(byte))
		{
			// This should never happen. Do something else if it does?
			WARN_LOG(EXPANSIONINTERFACE, "EXI SLIPPI: Invalid command byte: 0x%x", byte);
			return;
		}

		u32 payloadLen = payloadSizes[byte];
		switch (byte)
		{
		case CMD_RECEIVE_GAME_END:
			writeToFileAsync(&memPtr[bufLoc], payloadLen + 1, "close");
			break;
		case CMD_PREPARE_REPLAY:
			// log.open("log.txt");
			prepareGameInfo(&memPtr[bufLoc + 1]);
			break;
		case CMD_READ_FRAME:
			prepareFrameData(&memPtr[bufLoc + 1]);
			break;
		case CMD_IS_STOCK_STEAL:
			prepareIsStockSteal(&memPtr[bufLoc + 1]);
			break;
		case CMD_IS_FILE_READY:
			prepareIsFileReady();
			break;
		case CMD_GET_GECKO_CODES:
			m_read_queue.clear();
			m_read_queue.insert(m_read_queue.begin(), geckoList.begin(), geckoList.end());
			break;
		case CMD_ONLINE_INPUTS:
			handleOnlineInputs(&memPtr[bufLoc + 1]);
			break;
		case CMD_CAPTURE_SAVESTATE:
			handleCaptureSavestate(&memPtr[bufLoc + 1]);
			break;
		case CMD_LOAD_SAVESTATE:
			handleLoadSavestate(&memPtr[bufLoc + 1]);
			break;
		case CMD_GET_MATCH_STATE:
			prepareOnlineMatchState();
			break;
		case CMD_FIND_OPPONENT:
			startFindMatch(&memPtr[bufLoc + 1]);
			break;
		case CMD_SET_MATCH_SELECTIONS:
			setMatchSelections(&memPtr[bufLoc + 1]);
			break;
		case CMD_FILE_LENGTH:
			prepareFileLength(&memPtr[bufLoc + 1]);
			break;
		case CMD_FILE_LOAD:
			prepareFileLoad(&memPtr[bufLoc + 1]);
			break;
		case CMD_OPEN_LOGIN:
			handleLogInRequest();
			break;
		case CMD_LOGOUT:
			handleLogOutRequest();
			break;
		case CMD_GET_ONLINE_STATUS:
			prepareOnlineStatus();
			break;
		case CMD_CLEANUP_CONNECTION:
			handleConnectionCleanup();
			break;
		case CMD_LOG_MESSAGE:
			logMessageFromGame(&memPtr[bufLoc + 1]);
			break;
		case CMD_UPDATE:
			handleUpdateAppRequest();
			break;
		default:
			writeToFileAsync(&memPtr[bufLoc], payloadLen + 1, "");
			break;
		}

		bufLoc += payloadLen + 1;
	}
}

void CEXISlippi::DMARead(u32 addr, u32 size)
{
	if (m_read_queue.empty())
	{
		INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI DMARead: Empty");
		return;
	}

	m_read_queue.resize(size, 0); // Resize response array to make sure it's all full/allocated

	auto queueAddr = &m_read_queue[0];
	INFO_LOG(EXPANSIONINTERFACE, "EXI SLIPPI DMARead: addr: 0x%08x size: %d, startResp: [%02x %02x %02x %02x %02x]",
	         addr, size, queueAddr[0], queueAddr[1], queueAddr[2], queueAddr[3], queueAddr[4]);

	// Copy buffer data to memory
	Memory::CopyToEmu(addr, queueAddr, size);
}

bool CEXISlippi::IsPresent() const
{
	return true;
}

void CEXISlippi::TransferByte(u8 &byte) {}

void CEXISlippi::SavestateThread()
{
	Common::SetCurrentThreadName("Savestate thread");
	std::unique_lock<std::mutex> intervalLock(mtx);

	INFO_LOG(SLIPPI, "Entering savestate thread");

	while (g_playback_status->shouldRunThreads)
	{
		// Wait for signal to capture state
		condVar.wait(intervalLock);

		if (!g_playback_status->shouldRunThreads)
			break;

		int32_t fixedFrameNumber = g_playback_status->currentPlaybackFrame;
		if (fixedFrameNumber == INT_MAX)
			continue;

		bool isStartFrame = fixedFrameNumber == Slippi::GAME_FIRST_FRAME;
		bool hasStateBeenProcessed = futureDiffs.count(fixedFrameNumber) > 0;

		if (!g_playback_status->inSlippiPlayback && isStartFrame)
		{
			processInitialState(iState);
			g_playback_status->inSlippiPlayback = true;
		}
		else if (!hasStateBeenProcessed && !isStartFrame)
		{
			INFO_LOG(SLIPPI, "saving diff at frame: %d", fixedFrameNumber);
			State::SaveToBuffer(cState);

			futureDiffs[fixedFrameNumber] = std::async(processDiff, iState, cState);
		}
		Common::SleepCurrentThread(SLEEP_TIME_MS);
	}

	INFO_LOG(SLIPPI, "Exiting savestate thread");
}

void CEXISlippi::SeekThread()
{
	Common::SetCurrentThreadName("Seek thread");
	std::unique_lock<std::mutex> seekLock(seekMtx);

	INFO_LOG(SLIPPI, "Entering seek thread");

	while (g_playback_status->shouldRunThreads)
	{
		bool shouldSeek = g_playback_status->inSlippiPlayback &&
		                  (g_playback_status->shouldJumpBack || g_playback_status->shouldJumpForward ||
		                   g_playback_status->targetFrameNum != INT_MAX);

		if (shouldSeek)
		{
			// Clear start and end frames in queue mode
			auto replayCommSettings = replayComm->getSettings();
			if (replayCommSettings.mode == "queue")
				clearWatchSettingsStartEnd();

			bool paused = (Core::GetState() == Core::CORE_PAUSE);
			Core::SetState(Core::CORE_PAUSE);

			uint32_t jumpInterval = 300; // 5 seconds;

			if (g_playback_status->shouldJumpForward)
				g_playback_status->targetFrameNum = g_playback_status->currentPlaybackFrame + jumpInterval;

			if (g_playback_status->shouldJumpBack)
				g_playback_status->targetFrameNum = g_playback_status->currentPlaybackFrame - jumpInterval;

			// Handle edgecases for trying to seek before start or past end of game
			if (g_playback_status->targetFrameNum < Slippi::GAME_FIRST_FRAME)
				g_playback_status->targetFrameNum = Slippi::GAME_FIRST_FRAME;

			if (g_playback_status->targetFrameNum > g_playback_status->latestFrame)
			{
				g_playback_status->targetFrameNum = g_playback_status->latestFrame;
			}

			int32_t closestStateFrame =
			    g_playback_status->targetFrameNum - emod(g_playback_status->targetFrameNum + 123, FRAME_INTERVAL);

			bool isLoadingStateOptimal = g_playback_status->targetFrameNum < g_playback_status->currentPlaybackFrame ||
			                             closestStateFrame > g_playback_status->currentPlaybackFrame;

			if (isLoadingStateOptimal)
			{
				if (closestStateFrame <= Slippi::GAME_FIRST_FRAME)
				{
					State::LoadFromBuffer(iState);
				}
				else
				{
					// If this diff has been processed, load it
					if (futureDiffs.count(closestStateFrame) > 0)
					{
						std::string stateString;
						decoder.Decode((char *)iState.data(), iState.size(), futureDiffs[closestStateFrame].get(),
						               &stateString);
						std::vector<u8> stateToLoad(stateString.begin(), stateString.end());
						State::LoadFromBuffer(stateToLoad);
					};
				}
			}

			// Fastforward until we get to the frame we want
			if (g_playback_status->targetFrameNum != closestStateFrame &&
			    g_playback_status->targetFrameNum != g_playback_status->latestFrame)
			{
				isHardFFW = true;
				SConfig::GetInstance().m_OCEnable = true;
				SConfig::GetInstance().m_OCFactor = 4.0f;

				Core::SetState(Core::CORE_RUN);
				cv_waitingForTargetFrame.wait(seekLock);
				Core::SetState(Core::CORE_PAUSE);

				SConfig::GetInstance().m_OCFactor = 1.0f;
				SConfig::GetInstance().m_OCEnable = false;
				isHardFFW = false;
			}

			if (!paused)
				Core::SetState(Core::CORE_RUN);

			g_playback_status->shouldJumpBack = false;
			g_playback_status->shouldJumpForward = false;
			g_playback_status->targetFrameNum = INT_MAX;
		}

		Common::SleepCurrentThread(SLEEP_TIME_MS);
	}

	INFO_LOG(SLIPPI, "Exit seek thread");
}

void CEXISlippi::prepareSlippiPlayback(int32_t &frameIndex)
{
	// block if there's too many diffs being processed
	while (numDiffsProcessing > 3)
	{
		INFO_LOG(SLIPPI, "Processing too many diffs, blocking main process");
		cv_processingDiff.wait(processingLock);
	}

	if (g_playback_status->inSlippiPlayback && frameIndex == g_playback_status->targetFrameNum)
	{
		if (g_playback_status->targetFrameNum < g_playback_status->currentPlaybackFrame)
		{
			// Since playback logic only goes up in currentPlaybackFrame now due to handling rollback
			// playback, we need to rewind the currentPlaybackFrame here instead such that the playback
			// cursor will show up in the correct place
			g_playback_status->currentPlaybackFrame = g_playback_status->targetFrameNum;
		}

		INFO_LOG(SLIPPI, "Reached frame to seek to, unblock");
		cv_waitingForTargetFrame.notify_one();
	}

	// Unblock thread to save a state every interval
	if ((g_playback_status->currentPlaybackFrame - Slippi::GAME_FIRST_FRAME) % FRAME_INTERVAL == 0)
		condVar.notify_one();
}

void CEXISlippi::processInitialState(std::vector<u8> &iState)
{
	INFO_LOG(SLIPPI, "saving iState");
	State::SaveToBuffer(iState);
	SConfig::GetInstance().bHideCursor = false;
};

void CEXISlippi::resetPlayback()
{
	g_playback_status->shouldRunThreads = false;
	condVar.notify_one();                  // Will allow savestate thread to kill itself
	cv_waitingForTargetFrame.notify_one(); // Will allow seek thread to kill itself

	if (m_savestateThread.joinable())
		m_savestateThread.join();

	if (m_seekThread.joinable())
		m_seekThread.join();

	g_playback_status->shouldJumpBack = false;
	g_playback_status->shouldJumpForward = false;
	g_playback_status->targetFrameNum = INT_MAX;
	g_playback_status->inSlippiPlayback = false;
	futureDiffs.clear();
	futureDiffs.rehash(0);

	INFO_LOG(SLIPPI, "Done resetting playback");
}

void CEXISlippi::clearWatchSettingsStartEnd()
{
	int startFrame = replayComm->current.startFrame;
	int endFrame = replayComm->current.endFrame;
	if (startFrame != Slippi::GAME_FIRST_FRAME || endFrame != INT_MAX)
	{
		replayComm->current.startFrame = Slippi::GAME_FIRST_FRAME;
		replayComm->current.endFrame = INT_MAX;
	}
}
