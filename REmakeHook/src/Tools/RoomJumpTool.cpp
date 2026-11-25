#include "pch.h"

#include "Tools/RoomJumpTool.h"

#include "Game/GameAddress.h"
#include "Game/TargetRoomData.h"
#include "Utils/TrampHook.h"

#include "json11/json11.hpp"
#include "imgui/imgui.h"

#include <algorithm>
#include <map>
#include <random>
#include <string>
#include <unordered_map>

namespace
{
	struct RoomJumpPoint
	{
		float x;
		float y;
		float z;
		float angle;
		uint32_t cut;
		uint32_t page;
		uint32_t targetRoomNb;
		std::string name;
	};

	struct Room
	{
		std::string name;
		uint32_t roomNb;
		std::vector<RoomJumpPoint> jumpPoints;
	};

	struct DoorPortal
	{
		uint32_t roomNb;      // Room you land inside
		uint32_t fromRoomNb;  // Room you came from
		float x, y, z, angle;
		uint32_t cut, page;
	};

	std::vector<Room> rooms_;
	int selectedRoom_ = 0;
	int selectedJumpPoint_ = 0;

	int jumpRoomNb_ = 0;
	float jumpCoords_[3] = { 0.0f, 0.0f, 0.0f };
	float jumpAngle_ = 0.0f;
	uint32_t jumpCut_ = 0;
	uint32_t jumpPage_ = 0;
	bool jumpRoom_ = false;

	bool doorRandoEnabled_ = false;
	int doorRandoSeed_ = 1337;
	std::vector<DoorPortal> doorPortals_;
	std::unordered_map<uint64_t, DoorPortal> doorRedirectMap_; // key: ((uint64_t)vanillaDst << 32) | vanillaSrc -> newTarget

	using bhd_CheckForTriggers =  int(__thiscall*)(void*, int, uint32_t*, TargetRoomData*);
	bhd_CheckForTriggers checkForTriggers_ = nullptr;
	TrampHook checkForTriggersHook_;

	uint32_t ReadCurrentRoomNb()
	{
		if (!IsSeptember2026Build())
			return 0;
		uintptr_t savePtr = 0;
		SIZE_T br = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)GameAddresses[GAID_INVENTORY], &savePtr, sizeof(savePtr), &br) || savePtr == 0)
			return 0;
		uint32_t stage = 0, room = 0;
		ReadProcessMemory(GetCurrentProcess(), (void*)(savePtr + 0xe4750), &stage, sizeof(stage), &br);
		ReadProcessMemory(GetCurrentProcess(), (void*)(savePtr + 0xe4754), &room, sizeof(room), &br);
		return (stage << 8) | room;
	}

	void BuildDoorPortalList()
	{
		doorPortals_.clear();
		std::map<std::pair<uint32_t, uint32_t>, int> edgeCounts;
		for (const auto& r : rooms_)
		{
			for (const auto& jp : r.jumpPoints)
			{
				if (r.roomNb != jp.targetRoomNb)
					edgeCounts[{ r.roomNb, jp.targetRoomNb }]++;
			}
		}

		for (const auto& r : rooms_)
		{
			for (const auto& jp : r.jumpPoints)
			{
				if (r.roomNb == jp.targetRoomNb)
					continue;
				// Only include unambiguous 1-to-1 reciprocal edges
				if (edgeCounts[{ r.roomNb, jp.targetRoomNb }] == 1 && edgeCounts[{ jp.targetRoomNb, r.roomNb }] == 1)
				{
					doorPortals_.push_back({ r.roomNb, jp.targetRoomNb, jp.x, jp.y, jp.z, jp.angle, jp.cut, jp.page });
				}
			}
		}
	}

	void RebuildDoorRandomizer()
	{
		doorRedirectMap_.clear();
		if (!doorRandoEnabled_ || doorPortals_.size() < 2)
			return;

		// Build index lookup: (roomNb, fromRoomNb) -> index in doorPortals_
		std::unordered_map<uint64_t, size_t> portalIdx;
		for (size_t i = 0; i < doorPortals_.size(); ++i)
		{
			uint64_t k = ((uint64_t)doorPortals_[i].roomNb << 32) | doorPortals_[i].fromRoomNb;
			portalIdx[k] = i;
		}

		// Separate leaf portals (1-door rooms) from hub portals (multi-door rooms)
		// so two 1-door rooms never form an isolated 2-room island.
		std::unordered_map<uint32_t, int> roomPortalCount;
		for (const auto& dp : doorPortals_)
			roomPortalCount[dp.roomNb]++;

		std::vector<size_t> leaves, hubs;
		for (size_t i = 0; i < doorPortals_.size(); ++i)
		{
			if (roomPortalCount[doorPortals_[i].roomNb] <= 1)
				leaves.push_back(i);
			else
				hubs.push_back(i);
		}

		std::mt19937 rng(static_cast<uint32_t>(doorRandoSeed_));
		std::shuffle(leaves.begin(), leaves.end(), rng);
		std::shuffle(hubs.begin(), hubs.end(), rng);

		std::vector<size_t> partner(doorPortals_.size());
		for (size_t i = 0; i < partner.size(); ++i)
			partner[i] = i;

		size_t pairCount = (std::min)(leaves.size(), hubs.size());
		for (size_t i = 0; i < pairCount; ++i)
		{
			partner[leaves[i]] = hubs[i];
			partner[hubs[i]] = leaves[i];
		}

		std::vector<size_t> remaining;
		for (size_t i = pairCount; i < hubs.size(); ++i)
			remaining.push_back(hubs[i]);
		for (size_t i = pairCount; i < leaves.size(); ++i)
			remaining.push_back(leaves[i]);

		for (size_t i = 0; i + 1 < remaining.size(); i += 2)
		{
			partner[remaining[i]] = remaining[i + 1];
			partner[remaining[i + 1]] = remaining[i];
		}

		// When walking out of u (vanilla target is v = origPartner[u]), redirect to partner[u]
		for (size_t u = 0; u < doorPortals_.size(); ++u)
		{
			uint64_t vKey = ((uint64_t)doorPortals_[u].fromRoomNb << 32) | doorPortals_[u].roomNb;
			auto itV = portalIdx.find(vKey);
			if (itV != portalIdx.end())
			{
				// vKey is (dst=fromRoomNb, src=roomNb)
				doorRedirectMap_[vKey] = doorPortals_[partner[u]];
			}
		}
	}
}

namespace
{

void hk_bhd_FetchDoorRoomData(TargetRoomData* target, int roomData)
{
	float uVar1;
	float uVar2;

	target->stage = (uint32_t) * (byte*)(roomData + 0x40);
	target->room = (uint32_t) * (byte*)(roomData + 0x41);
	target->cut = (int)*(char*)(roomData + 0x42);
	uVar1 = *(float*)(roomData + 0x34);
	uVar2 = *(float*)(roomData + 0x38);
	target->x = *(float*)(roomData + 0x30);
	target->y = uVar1;
	target->z = uVar2;
	target->page = 0;
	target->angle = *(float*)(roomData + 0x48);

	//bhd_FetchDoorRoomDataFunc_(target, roomData);
}

int __fastcall hk_bhd_CheckForTriggers(void* obj, void* edx, int param_1, uint32_t* param_2, TargetRoomData* target)
{
	if (jumpRoom_)
	{
		target->stage = jumpRoomNb_ / 0x100;
		target->room = jumpRoomNb_ % 0x100;
		target->cut = jumpCut_;
		target->x = jumpCoords_[0];
		target->y = jumpCoords_[1];
		target->z = jumpCoords_[2];
		target->page = jumpPage_;
		target->angle = (jumpAngle_ / 180.0f) * PI;

		jumpRoom_ = false;
		return 1;
	}
	else
	{
		uint32_t srcRoomNb = ReadCurrentRoomNb();
		int ret = checkForTriggers_(obj, param_1, param_2, target);
		if (ret == 4 && doorRandoEnabled_ && target != nullptr && srcRoomNb != 0)
		{
			uint32_t dstRoomNb = (target->stage << 8) | target->room;
			uint64_t key = ((uint64_t)dstRoomNb << 32) | srcRoomNb;
			auto it = doorRedirectMap_.find(key);
			if (it != doorRedirectMap_.end())
			{
				const DoorPortal& dest = it->second;
				target->stage = dest.roomNb / 0x100;
				target->room = dest.roomNb % 0x100;
				target->cut = dest.cut;
				target->x = dest.x;
				target->y = dest.y;
				target->z = dest.z;
				target->page = dest.page;
				target->angle = (dest.angle / 180.0f) * PI;
			}
		}
		return ret;
	}
}

}

void RoomJumpTool::Init()
{
	checkForTriggersHook_.Set((char*)GameAddresses[GAID_CHECK_FOR_TRIGGERS], (char*)&hk_bhd_CheckForTriggers, 9);
	checkForTriggersHook_.Apply();
	checkForTriggers_ = (bhd_CheckForTriggers)checkForTriggersHook_.GetGateway();

	uintptr_t funcAdd = (uintptr_t)(&hk_bhd_FetchDoorRoomData);
	funcAdd = funcAdd - GameAddresses[GAID_FETCH_DOOR_ROOM_DATA_OFFSET] - 5;
	uint8_t* funcAdd8 = (uint8_t*)(&funcAdd);

	const size_t codeAddress = GameAddresses[GAID_FETCH_DOOR_ROOM_DATA];
	CodePatch cp;
	cp.AddCode(codeAddress, { 0x8b, 0x44, 0x24, 0x08 });
	cp.AddCode(codeAddress + 0x4, { 0x8b, 0x4c, 0x24, 0x04 });
	cp.AddCode(codeAddress + 0x8, { 0x50 });
	cp.AddCode(codeAddress + 0x9, { 0x51 });
	cp.AddCode(codeAddress + 0xA, { 0xE8, funcAdd8[0], funcAdd8[1], funcAdd8[2], funcAdd8[3] });
	cp.AddCode(codeAddress + 0xF, { 0x83, 0xC4, 0x08 });
	cp.AddCode(codeAddress + 0x12, { 0xC2, 0x08, 0x00 });
	cp.AddCode(codeAddress + 0x15, { 0x90 });
	cp.Apply();


	std::string jsonStr;
	FILE* jsonFile = nullptr;
	if (fopen_s(&jsonFile, "rooms.json", "rb") == 0)
	{
		fseek(jsonFile, 0, SEEK_END);
		const size_t jsonFileSize = ftell(jsonFile);
		fseek(jsonFile, 0, SEEK_SET);
		jsonStr.resize(jsonFileSize);
		fread(jsonStr.data(), 1, jsonFileSize, jsonFile);
		fclose(jsonFile);
	}

	std::string jsonErrors;
	json11::Json roomsJson = json11::Json::parse(jsonStr, jsonErrors);
	for (const auto& roomJson : roomsJson.object_items())
	{
		Room room;
		sscanf_s(roomJson.first.c_str(), "%x", &room.roomNb);
		room.name = roomJson.second["displayName"].string_value();
		for (const auto& jumpPointJson : roomJson.second["doors"].array_items())
		{
			RoomJumpPoint jumpPoint;
			jumpPoint.x = static_cast<float>(jumpPointJson["x"].number_value());
			jumpPoint.y = static_cast<float>(jumpPointJson["y"].number_value());
			jumpPoint.z = static_cast<float>(jumpPointJson["z"].number_value());
			jumpPoint.angle = static_cast<float>(jumpPointJson["angle"].number_value());
			jumpPoint.cut = jumpPointJson["cut"].int_value();
			jumpPoint.page = jumpPointJson["page"].int_value();
			const std::string& targetRoomNbStr = jumpPointJson["targetRoomNb"].string_value();
			sscanf_s(targetRoomNbStr.c_str(), "%x", &jumpPoint.targetRoomNb);
			room.jumpPoints.push_back(jumpPoint);
		}
		rooms_.push_back(room);
	}

	std::sort(rooms_.begin(), rooms_.end(), [](const Room& a, const Room& b)
	{
		return a.name.compare(b.name) < 0;
	});

	for (Room& room : rooms_)
	{
		for (RoomJumpPoint& jumpPoint : room.jumpPoints)
		{
			auto it = std::find_if(rooms_.begin(), rooms_.end(), [&jumpPoint](const Room& a)
			{
				return a.roomNb == jumpPoint.targetRoomNb;
			});
			if (it != rooms_.end())
			{
				jumpPoint.name = it->name;
			}
		}
	}

	BuildDoorPortalList();
	RebuildDoorRandomizer();

	/*FunctionHook hook3;
	hook3.Set(0x0041e06a, &hk_bhd_FetchDoorRoomData);
	hook3.Apply();*/

	/*fetchDoorRoomDataHook_.Set((char*)0x0041e340, (char*)&hk_bhd_FetchDoorRoomData, 8);
	fetchDoorRoomDataHook_.Apply();/*
	bhd_FetchDoorRoomDataFunc_ = (bhd_FetchDoorRoomData)fetchDoorRoomDataHook_.GetGateway();*/

	/*checkTriggersHook_.Set((char*)0x0040a200, (char*)&hk_bhd_CheckTriggers, 7);
	checkTriggersHook_.Apply();

	bhd_CheckDoor = (uint32_t(__stdcall*)(float*, int))checkTriggersHook_.GetGateway();*/
}

void RoomJumpTool::UpdateUI()
{
	if (ImGui::CollapsingHeader("Room Jump"))
	{
		ImGui::Indent();
		if (!rooms_.empty())
		{
			if (ImGui::BeginCombo("Room", rooms_[selectedRoom_].name.c_str()))
			{
				for (int i = 0; i < static_cast<int>(rooms_.size()); ++i)
				{
					bool selected = i == selectedRoom_;
					if (ImGui::Selectable(rooms_[i].name.c_str(), &selected))
					{
						selectedRoom_ = i;
						selectedJumpPoint_ = 0;
					}
				}
				ImGui::EndCombo();
			}
			ImGui::Text("Doors");
			const Room& room = rooms_[selectedRoom_];
			for (int i = 0; i < static_cast<int>(room.jumpPoints.size()); ++i)
			{
				if (i % 4 != 0)
				{
					ImGui::SameLine();
				}
				if (ImGui::RadioButton(room.jumpPoints[i].name.c_str(), i == selectedJumpPoint_))
				{
					selectedJumpPoint_ = i;
				}
			}

			static bool manualEnter = false;
			ImGui::Checkbox("Set coords manually", &manualEnter);
			if (manualEnter)
			{
				ImGui::InputFloat3("Coords", jumpCoords_);
				ImGui::InputFloat("Angle", &jumpAngle_);
				jumpAngle_ = std::clamp(jumpAngle_, 0.0f, 360.0f);
			}

			if (ImGui::Button("Jump"))
			{
				const Room& room = rooms_[selectedRoom_];
				const RoomJumpPoint& jumpPoint = room.jumpPoints[selectedJumpPoint_];
				jumpRoomNb_ = room.roomNb;

				if (!manualEnter)
				{
					jumpCoords_[0] = jumpPoint.x;
					jumpCoords_[1] = jumpPoint.y;
					jumpCoords_[2] = jumpPoint.z;
					jumpAngle_ = jumpPoint.angle;
					jumpCut_ = jumpPoint.cut;
					jumpPage_ = jumpPoint.page;
				}
				else
				{
					jumpCut_ = 0;
					jumpPage_ = 0;
				}
				jumpRoom_ = true;
			}
		}
		ImGui::Unindent();
	}
}

std::string RoomJumpTool::GetRoomName(uint32_t roomNb)
{
	for (const auto& r : rooms_)
	{
		if (r.roomNb == roomNb)
			return r.name;
	}
	char buf[32];
	sprintf_s(buf, "Room 0x%X", roomNb);
	return std::string(buf);
}

void RoomJumpTool::SetDoorRandomizer(bool enabled, int seed)
{
	doorRandoEnabled_ = enabled;
	doorRandoSeed_ = seed;
	RebuildDoorRandomizer();
}

int RoomJumpTool::GetDoorRandomizerCount()
{
	return static_cast<int>(doorRedirectMap_.size());
}