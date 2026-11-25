#pragma once

#include <cstdint>
#include <string>

namespace RoomJumpTool
{
	void Init();
	void UpdateUI();

	std::string GetRoomName(uint32_t roomNb);
	void SetDoorRandomizer(bool enabled, int seed);
	int GetDoorRandomizerCount();
}
