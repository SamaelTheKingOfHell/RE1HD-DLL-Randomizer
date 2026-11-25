#include "pch.h"

#include "Tools/RandomizerTool.h"
#include "Tools/RoomJumpTool.h"
#include "Tools/VanillaItemTable.h"
#include "Game/GameAddress.h"
#include "Game/Items.h"
#include "Utils/TrampHook.h"
#include "imgui/imgui.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <fstream>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
	struct ReplItem
	{
		int itemId;
		int qty;
	};

	struct SpawnLogEntry
	{
		uint32_t roomNb;
		int flag;
		int origItem;
		int origQty;
		int newItem;
		int newQty;
	};

	TrampHook spawnMainHook_;
	TrampHook spawnAltHook_;
	TrampHook spawnUItemModelHook_;
	bool hooksInstalled_ = false;

	// Thread-local guard so SpawnUItemModel keeps the room's already-mounted 3D floor model
	// and never crashes trying to open unmounted iXXXp.mod files (e.g. i004p.mod).
	thread_local bool tl_overrideFloorModel_ = false;
	thread_local int tl_origFloorItemId_ = 0;

	// Benn Powell-style options
	int playAsMode_ = 0;      // 0 = Auto-Detect, 1 = Chris, 2 = Jill
	int difficultyMode_ = 0;  // 0 = Auto-Detect, 1 = Normal, 2 = Hard
	int itemMode_ = 1;        // 0 = Off (Log Only), 1 = Safe Shuffle, 2 = Full Shuffle
	bool randomizeDoors_ = false;

	// Remove from pool (replace with Junk: WoodenMount, SlideFilter, XRay1, XRay2)
	bool removeHealing_ = false;
	bool removeInkRibbons_ = false;
	bool removeDefence_ = false;

	// Modifiers
	bool swapAllWithSelfDefenseGun_ = false;
	bool fuelCapsuleArmedOnPickup_ = false;

	int seed_ = 1337;
	std::unordered_map<int, ReplItem> shuffleMap_;
	std::deque<SpawnLogEntry> recentSpawns_;
	char filterBuf_[64] = "";
	std::string exportStatus_;

	using SpawnMainFn = void* (__thiscall*)(void* obj, int itemId, float* pos, float angle, int qty, int pickupFlag, int slotIndex, uint32_t flags, uint32_t unk, int modelType);
	using SpawnAltFn = void* (__thiscall*)(void* obj, int itemId, float* pos, float angle, int qty, int pickupFlag, int slotIndex, int modelType, uint32_t flags);
	using SpawnUItemModelFn = void* (__thiscall*)(void* obj, int itemId, int modelType, float* pos, float angle);

	uintptr_t GetSavePtr()
	{
		if (!IsSeptember2026Build())
			return 0;
		uintptr_t savePtr = 0;
		SIZE_T br = 0;
		if (!ReadProcessMemory(GetCurrentProcess(), (void*)GameAddresses[GAID_INVENTORY], &savePtr, sizeof(savePtr), &br))
			return 0;
		return savePtr;
	}

	uint32_t GetCurrentRoomNb()
	{
		uintptr_t savePtr = GetSavePtr();
		if (!savePtr)
			return 0;
		uint32_t stage = 0, room = 0;
		SIZE_T br = 0;
		ReadProcessMemory(GetCurrentProcess(), (void*)(savePtr + 0xe4750), &stage, sizeof(stage), &br);
		ReadProcessMemory(GetCurrentProcess(), (void*)(savePtr + 0xe4754), &room, sizeof(room), &br);
		return (stage << 8) | room;
	}

	int GetDetectedCharacter()
	{
		uintptr_t savePtr = GetSavePtr();
		if (!savePtr)
			return 0;
		int ch = 0;
		SIZE_T br = 0;
		ReadProcessMemory(GetCurrentProcess(), (void*)(savePtr + 0x5110), &ch, sizeof(ch), &br);
		return ch; // 0 = Chris, 1 = Jill
	}

	int GetDetectedDifficulty()
	{
		uintptr_t savePtr = GetSavePtr();
		if (!savePtr)
			return 0;
		int diff = 0;
		SIZE_T br = 0;
		ReadProcessMemory(GetCurrentProcess(), (void*)(savePtr + 0x24), &diff, sizeof(diff), &br);
		return diff;
	}

	bool IsEffectiveJill()
	{
		if (playAsMode_ == 1) return false;
		if (playAsMode_ == 2) return true;
		return GetDetectedCharacter() == 1;
	}

	bool IsHealingItem(int itemId)
	{
		switch (static_cast<ItemId>(itemId))
		{
		case ItemId::FirstAidSpray:
		case ItemId::GreenHerb:
		case ItemId::RedHerb:
		case ItemId::BlueHerb:
		case ItemId::Green2HerbsMix:
		case static_cast<ItemId>(38): // Green3HerbsMix
		case ItemId::GreenRedHerbsMix:
		case ItemId::GreenRedBlueHerbsMix:
		case ItemId::GreebBlueHerbsMix:
		case ItemId::Green2BlueHerbsMix:
		case ItemId::FirstAidBoxSpray:
		case ItemId::FirstAidfBoxGreen:
		case ItemId::FirstAidfBoxBlue:
		case ItemId::FirstAidBoxGreenBlue:
		case ItemId::FirstAidBoxGreen2:
			return true;
		default:
			return false;
		}
	}

	bool IsDefenceItem(int itemId)
	{
		switch (static_cast<ItemId>(itemId))
		{
		case ItemId::Dagger:
		case ItemId::FlashGrenade:
		case ItemId::StunGun:
		case ItemId::BatteryPack:
			return true;
		default:
			return false;
		}
	}

	bool IsNonKeyItem(int itemId)
	{
		if (IsHealingItem(itemId) || IsDefenceItem(itemId))
			return true;
		switch (static_cast<ItemId>(itemId))
		{
		case ItemId::Pistol:
		case ItemId::SelfDefenseGun:
		case ItemId::SamuraiEdge:
		case ItemId::Shotgun:
		case ItemId::GrenadeLauncherNormal:
		case ItemId::GrenadeLauncherAcid:
		case ItemId::GrenadeLauncherIncendiary:
		case ItemId::Magnum:
		case ItemId::FlameThrower:
		case ItemId::AssaultShotgun:
		case ItemId::GrenadeShells:
		case ItemId::HandgunMagazine:
		case ItemId::ShotgunShells:
		case ItemId::AcidShells:
		case ItemId::IncendiaryShells:
		case ItemId::MagnumRounds:
		case ItemId::InkRibbon:
			return true;
		default:
			return false;
		}
	}

	int PickJunkItem(std::mt19937& rng)
	{
		static constexpr int JunkPool[] = {
			static_cast<int>(ItemId::WoodenMount),
			static_cast<int>(ItemId::SlideFilter),
			static_cast<int>(ItemId::XRay1),
			static_cast<int>(ItemId::XRay2)
		};
		return JunkPool[rng() % 4];
	}

	void RebuildShuffleMap()
	{
		shuffleMap_.clear();
		RoomJumpTool::SetDoorRandomizer(randomizeDoors_, seed_);

		if (itemMode_ == 0 && !removeHealing_ && !removeInkRibbons_ && !removeDefence_ && !swapAllWithSelfDefenseGun_ && !fuelCapsuleArmedOnPickup_)
			return;

		std::mt19937 rng(static_cast<uint32_t>(seed_));
		const bool isJill = IsEffectiveJill();

		std::vector<int> flags;
		std::vector<ReplItem> pool;

		for (const auto& vp : VanillaPickups)
		{
			// Jill natively skips SmallKey (44) inside SpawnRoomItemMain; never replace with SmallKey for Jill
			if (isJill && vp.itemId == static_cast<int>(ItemId::SmallKey))
				continue;

			int outId = vp.itemId;
			int outQty = vp.qty;

			if (swapAllWithSelfDefenseGun_ && IsNonKeyItem(outId))
			{
				outId = static_cast<int>(ItemId::SelfDefenseGun);
				outQty = 1;
			}
			else
			{
				if (removeHealing_ && IsHealingItem(outId))
				{
					outId = PickJunkItem(rng);
					outQty = 1;
				}
				else if (removeInkRibbons_ && outId == static_cast<int>(ItemId::InkRibbon))
				{
					outId = PickJunkItem(rng);
					outQty = 1;
				}
				else if (removeDefence_ && IsDefenceItem(outId))
				{
					outId = PickJunkItem(rng);
					outQty = 1;
				}
			}

			if (fuelCapsuleArmedOnPickup_ && outId == static_cast<int>(ItemId::FuelCapsuleEmpty))
			{
				outId = static_cast<int>(ItemId::FuelCapsuleArmed);
			}

			if (itemMode_ == 1 && !IsNonKeyItem(vp.itemId))
			{
				// Safe Shuffle: key/progression items stay in their vanilla spots (applying only capsule modifier)
				if (outId != vp.itemId || outQty != vp.qty)
					shuffleMap_[vp.flag] = { outId, outQty };
				continue;
			}

			if (itemMode_ == 0)
			{
				if (outId != vp.itemId || outQty != vp.qty)
					shuffleMap_[vp.flag] = { outId, outQty };
				continue;
			}

			flags.push_back(vp.flag);
			pool.push_back({ outId, outQty });
		}

		if (itemMode_ != 0 && !pool.empty())
		{
			std::shuffle(pool.begin(), pool.end(), rng);
			for (size_t i = 0; i < flags.size(); ++i)
			{
				shuffleMap_[flags[i]] = pool[i];
			}
		}
	}

	const char* GetItemNameSafe(int itemId)
	{
		if (itemId >= 0 && itemId < ItemCount)
			return ItemNames[itemId];
		return "Unknown";
	}

	void ExportSpoilerLog()
	{
		std::ofstream out("RandomizerSpoiler.txt");
		if (!out.is_open())
		{
			exportStatus_ = "Failed to write RandomizerSpoiler.txt";
			return;
		}
		out << "RE1 HD Randomizer (dinput8.dll) - Seed: " << seed_
			<< " | ItemMode: " << itemMode_
			<< " | Doors: " << (randomizeDoors_ ? "On" : "Off") << "\n";
		out << "Flag\tVanilla\tReplacedWith\n";
		for (const auto& vp : VanillaPickups)
		{
			auto it = shuffleMap_.find(vp.flag);
			int newId = (it != shuffleMap_.end()) ? it->second.itemId : vp.itemId;
			int newQty = (it != shuffleMap_.end()) ? it->second.qty : vp.qty;
			out << vp.flag << "\t"
				<< GetItemNameSafe(vp.itemId) << " (x" << vp.qty << ")\t"
				<< GetItemNameSafe(newId) << " (x" << newQty << ")\n";
		}
		exportStatus_ = "Exported RandomizerSpoiler.txt";
	}

	void ApplyRandomization(int& itemId, int& qty, int pickupFlag, int& origOut)
	{
		origOut = itemId;
		int origQty = qty;

		if (itemId > 0 && itemId < ItemCount)
		{
			if (IsEffectiveJill() && origOut == static_cast<int>(ItemId::SmallKey))
			{
				// Let vanilla Jill logic handle/skip SmallKey safely
				return;
			}

			auto it = shuffleMap_.find(pickupFlag);
			if (it != shuffleMap_.end())
			{
				itemId = it->second.itemId;
				qty = it->second.qty;
			}
			else if (fuelCapsuleArmedOnPickup_ && itemId == static_cast<int>(ItemId::FuelCapsuleEmpty))
			{
				itemId = static_cast<int>(ItemId::FuelCapsuleArmed);
			}
		}

		recentSpawns_.push_front({ GetCurrentRoomNb(), pickupFlag, origOut, origQty, itemId, qty });
		if (recentSpawns_.size() > 25)
			recentSpawns_.pop_back();
	}

	void* __fastcall hk_SpawnRoomItemMain(void* obj, void* edx, int itemId, float* pos, float angle, int qty, int pickupFlag, int slotIndex, uint32_t flags, uint32_t unk, int modelType)
	{
		int origItem = itemId;
		ApplyRandomization(itemId, qty, pickupFlag, origItem);

		tl_origFloorItemId_ = origItem;
		tl_overrideFloorModel_ = (itemId != origItem);

		SpawnMainFn orig = (SpawnMainFn)spawnMainHook_.GetGateway();
		void* result = orig(obj, itemId, pos, angle, qty, pickupFlag, slotIndex, flags, unk, modelType);

		tl_overrideFloorModel_ = false;
		return result;
	}

	void* __fastcall hk_SpawnRoomItemAlt(void* obj, void* edx, int itemId, float* pos, float angle, int qty, int pickupFlag, int slotIndex, int modelType, uint32_t flags)
	{
		int origItem = itemId;
		ApplyRandomization(itemId, qty, pickupFlag, origItem);

		tl_origFloorItemId_ = origItem;
		tl_overrideFloorModel_ = (itemId != origItem);

		SpawnAltFn orig = (SpawnAltFn)spawnAltHook_.GetGateway();
		void* result = orig(obj, itemId, pos, angle, qty, pickupFlag, slotIndex, modelType, flags);

		tl_overrideFloorModel_ = false;
		return result;
	}

	void* __fastcall hk_SpawnUItemModel(void* obj, void* edx, int itemId, int modelType, float* pos, float angle)
	{
		if (tl_overrideFloorModel_ && tl_origFloorItemId_ > 0 && tl_origFloorItemId_ < ItemCount)
		{
			itemId = tl_origFloorItemId_;
		}
		SpawnUItemModelFn orig = (SpawnUItemModelFn)spawnUItemModelHook_.GetGateway();
		return orig(obj, itemId, modelType, pos, angle);
	}
}

void RandomizerTool::Init()
{
	RebuildShuffleMap();

	if (IsSeptember2026Build()
		&& GameAddresses[GAID_SPAWN_ROOM_ITEM_MAIN] != 0
		&& GameAddresses[GAID_SPAWN_ROOM_ITEM_ALT] != 0
		&& GameAddresses[GAID_SPAWN_UITEM_MODEL] != 0)
	{
		spawnMainHook_.Set((char*)GameAddresses[GAID_SPAWN_ROOM_ITEM_MAIN], (char*)&hk_SpawnRoomItemMain, 6);
		spawnMainHook_.Apply();

		spawnAltHook_.Set((char*)GameAddresses[GAID_SPAWN_ROOM_ITEM_ALT], (char*)&hk_SpawnRoomItemAlt, 6);
		spawnAltHook_.Apply();

		spawnUItemModelHook_.Set((char*)GameAddresses[GAID_SPAWN_UITEM_MODEL], (char*)&hk_SpawnUItemModel, 5);
		spawnUItemModelHook_.Apply();

		hooksInstalled_ = true;
	}
}

void RandomizerTool::UpdateUI()
{
	if (ImGui::CollapsingHeader("Randomizer (dinput8.dll)"))
	{
		ImGui::Indent();

		if (!hooksInstalled_)
		{
			ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Hooks inactive (unsupported build)");
			ImGui::Unindent();
			return;
		}

		const uint32_t curRoom = GetCurrentRoomNb();
		const std::string curRoomName = curRoom ? RoomJumpTool::GetRoomName(curRoom) : "Main Menu";
		ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "Hooks ACTIVE | %s (0x%X)", curRoomName.c_str(), curRoom);

		const char* playAsOpts[] = { "Auto-Detect", "Chris", "Jill" };
		const char* diffOpts[] = { "Auto-Detect", "Normal", "Hard" };
		bool dirty = false;

		ImGui::PushItemWidth(160);
		if (ImGui::Combo("Play As", &playAsMode_, playAsOpts, IM_ARRAYSIZE(playAsOpts))) dirty = true;
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", GetDetectedCharacter() == 1 ? "Jill" : "Chris");

		if (ImGui::Combo("Difficulty", &difficultyMode_, diffOpts, IM_ARRAYSIZE(diffOpts))) dirty = true;
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", GetDetectedDifficulty() >= 2 ? "Hard+" : "Normal");
		ImGui::PopItemWidth();

		ImGui::Separator();
		const char* modes[] = { "Off (Log Only)", "Safe Shuffle (Keep Key Items)", "Full Shuffle (All Items)" };
		if (ImGui::Combo("Randomize Items", &itemMode_, modes, IM_ARRAYSIZE(modes))) dirty = true;
		if (ImGui::Checkbox("Randomize Doors (Bidirectional)", &randomizeDoors_)) dirty = true;
		if (randomizeDoors_)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%d paired portals)", RoomJumpTool::GetDoorRandomizerCount());
		}

		ImGui::Separator();
		ImGui::Text("Remove from pool (replace with Junk):");
		if (ImGui::Checkbox("Healing Items", &removeHealing_)) dirty = true;
		ImGui::SameLine();
		if (ImGui::Checkbox("Ink Ribbons", &removeInkRibbons_)) dirty = true;
		ImGui::SameLine();
		if (ImGui::Checkbox("Defence Items", &removeDefence_)) dirty = true;

		ImGui::Separator();
		ImGui::Text("Modifiers:");
		if (ImGui::Checkbox("Swap all non-key items with Self-Defense Gun", &swapAllWithSelfDefenseGun_)) dirty = true;
		if (ImGui::Checkbox("Fuel Supply Capsule armed on pickup", &fuelCapsuleArmedOnPickup_)) dirty = true;

		ImGui::Separator();
		if (ImGui::InputInt("Seed", &seed_)) dirty = true;
		ImGui::SameLine();
		if (ImGui::Button("New Seed"))
		{
			std::random_device rd;
			seed_ = static_cast<int>(rd() & 0x7FFFFFFF);
			dirty = true;
		}
		ImGui::SameLine();
		if (ImGui::Button("Export Spoiler"))
		{
			ExportSpoilerLog();
		}

		if (dirty)
		{
			RebuildShuffleMap();
		}

		ImGui::Text("Shuffled item flags: %zu / %zu", shuffleMap_.size(), VanillaPickups.size());
		if (!exportStatus_.empty())
		{
			ImGui::Text("%s", exportStatus_.c_str());
		}
		ImGui::TextDisabled("Note: Enemy & Lock randomization not yet hooked (prevents unmounted ARC crashes).");

		if (!recentSpawns_.empty() && ImGui::TreeNode("Recent Room Spawns"))
		{
			for (const auto& s : recentSpawns_)
			{
				const std::string rn = RoomJumpTool::GetRoomName(s.roomNb);
				ImGui::Text("[%s | Flag %d] %s (x%d) -> %s (x%d)",
					rn.c_str(), s.flag,
					GetItemNameSafe(s.origItem), s.origQty,
					GetItemNameSafe(s.newItem), s.newQty);
			}
			ImGui::TreePop();
		}

		if (ImGui::TreeNode("Content Map (All 153 Pickups)"))
		{
			ImGui::InputText("Filter Item", filterBuf_, sizeof(filterBuf_));
			std::string filterStr(filterBuf_);
			std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);

			ImGui::BeginChild("ContentMapScroll", ImVec2(0, 220), true);
			for (const auto& vp : VanillaPickups)
			{
				auto it = shuffleMap_.find(vp.flag);
				int newId = (it != shuffleMap_.end()) ? it->second.itemId : vp.itemId;
				int newQty = (it != shuffleMap_.end()) ? it->second.qty : vp.qty;

				std::string origName = GetItemNameSafe(vp.itemId);
				std::string newName = GetItemNameSafe(newId);

				if (!filterStr.empty())
				{
					std::string loOrig = origName, loNew = newName;
					std::transform(loOrig.begin(), loOrig.end(), loOrig.begin(), ::tolower);
					std::transform(loNew.begin(), loNew.end(), loNew.begin(), ::tolower);
					if (loOrig.find(filterStr) == std::string::npos && loNew.find(filterStr) == std::string::npos)
						continue;
				}

				ImGui::Text("[Flag %3d] %s (x%d) -> %s (x%d)",
					vp.flag, origName.c_str(), vp.qty, newName.c_str(), newQty);
			}
			ImGui::EndChild();
			ImGui::TreePop();
		}

		ImGui::Unindent();
	}
}
