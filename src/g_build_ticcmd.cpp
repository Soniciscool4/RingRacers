// DR. ROBOTNIK'S RING RACERS
//-----------------------------------------------------------------------------
// Copyright (C) 1993-1996 by id Software, Inc.
// Copyright (C) 1998-2000 by DooM Legacy Team.
// Copyright (C) 1999-2020 by Sonic Team Junior.
// Copyright (C) 2023      by Kart Krew.
//
// This program is free software distributed under the
// terms of the GNU General Public License, version 2.
// See the 'LICENSE' file for more details.
//-----------------------------------------------------------------------------

#include <algorithm>
#include <cmath>

#include "console.h"
#include "d_player.h"
#include "d_ticcmd.h"
#include "doomstat.h"
#include "doomtype.h"
#include "g_demo.h"
#include "g_game.h"
#include "g_input.h"
#include "g_state.h"
#include "g_party.h"
#include "hu_stuff.h"
#include "i_joy.h"
#include "i_system.h"
#include "k_bot.h"
#include "k_director.h"
#include "k_kart.h"
#include "k_menu.h"
#include "lua_hook.h"
#include "m_cheat.h"
#include "m_fixed.h"
#include "p_local.h"
#include "p_mobj.h"
#include "p_tick.h"
#include "tables.h"

namespace
{

// Take a magnitude of two axes, and adjust it to take out the deadzone
// Will return a value between 0 and JOYAXISRANGE
INT32 G_BasicDeadZoneCalculation(INT32 magnitude, fixed_t deadZone)
{
	const INT32 jdeadzone = (JOYAXISRANGE * deadZone) / FRACUNIT;

	INT32 adjustedMagnitude = std::abs(magnitude);

	if (jdeadzone >= JOYAXISRANGE && adjustedMagnitude >= JOYAXISRANGE) // If the deadzone and magnitude are both 100%...
	{
		return JOYAXISRANGE; // ...return 100% input directly, to avoid dividing by 0
	}

	if (adjustedMagnitude <= jdeadzone)
	{
		return 0; // Magnitude is within deadzone, so do nothing
	}

	// Calculate how much the magnitude exceeds the deadzone
	adjustedMagnitude = std::min(adjustedMagnitude, JOYAXISRANGE) - jdeadzone;
	return (adjustedMagnitude * JOYAXISRANGE) / (JOYAXISRANGE - jdeadzone);
}

class TiccmdBuilder
{
	struct JoyStickVector2
	{
		INT32 xaxis;
		INT32 yaxis;
	};

	ticcmd_t* cmd;
	INT32 realtics;
	UINT8 ssplayer;
	UINT8 viewnum;
	JoyStickVector2 joystickvector;

	UINT8 forplayer() const { return ssplayer - 1; }
	player_t* player() const { return &players[g_localplayers[forplayer()]]; }

	// Get the actual sensible radial value for a joystick axis when accounting for a deadzone
	void handle_axis_deadzone()
	{
		INT32 gamepadStyle = Joystick[forplayer()].bGamepadStyle;
		fixed_t deadZone = cv_deadzone[forplayer()].value;

		// When gamepadstyle is "true" the values are just -1, 0, or 1. This is done in the interface code.
		if (gamepadStyle)
		{
			return;
		}

		// Get the total magnitude of the 2 axes
		INT32 magnitude = std::sqrt(static_cast<double>(
				(joystickvector.xaxis * joystickvector.xaxis) + (joystickvector.yaxis * joystickvector.yaxis)
		));

		// Get the normalised xy values from the magnitude
		INT32 normalisedXAxis = (joystickvector.xaxis * magnitude) / JOYAXISRANGE;
		INT32 normalisedYAxis = (joystickvector.yaxis * magnitude) / JOYAXISRANGE;

		// Apply the deadzone to the magnitude to give a correct value between 0 and JOYAXISRANGE
		INT32 normalisedMagnitude = G_BasicDeadZoneCalculation(magnitude, deadZone);

		// Apply the deadzone to the xy axes
		joystickvector.xaxis = (normalisedXAxis * normalisedMagnitude) / JOYAXISRANGE;
		joystickvector.yaxis = (normalisedYAxis * normalisedMagnitude) / JOYAXISRANGE;

		// Cap the values so they don't go above the correct maximum
		joystickvector.xaxis = std::min(joystickvector.xaxis, JOYAXISRANGE);
		joystickvector.xaxis = std::max(joystickvector.xaxis, -JOYAXISRANGE);
		joystickvector.yaxis = std::min(joystickvector.yaxis, JOYAXISRANGE);
		joystickvector.yaxis = std::max(joystickvector.yaxis, -JOYAXISRANGE);
	}

	void hook()
	{
		/*
		Lua: Allow this hook to overwrite ticcmd.
		We check if we're actually in a level because for some reason this Hook would run in menus and on the titlescreen otherwise.
		Be aware that within this hook, nothing but this player's cmd can be edited (otherwise we'd run in some pretty bad synching problems since this is clientsided, or something)

		Possible usages for this are:
			-Forcing the player to perform an action, which could otherwise require terrible, terrible hacking to replicate.
			-Preventing the player to perform an action, which would ALSO require some weirdo hacks.
			-Making some galaxy brain autopilot Lua if you're a masochist
			-Making a Mario Kart 8 Deluxe tier baby mode that steers you away from walls and whatnot. You know what, do what you want!
		*/

		if (!addedtogame || gamestate != GS_LEVEL)
		{
			return;
		}

		LUA_HookTiccmd(player(), cmd, HOOK(PlayerCmd));

		auto clamp = [](auto val, int range) { return std::clamp(static_cast<int>(val), -(range), range); };

		cmd->forwardmove = clamp(cmd->forwardmove, MAXPLMOVE);
		cmd->turning = clamp(cmd->turning, KART_FULLTURN);
		cmd->throwdir = clamp(cmd->throwdir, KART_FULLTURN);

		// Send leveltime when this tic was generated to the server for control lag calculations.
		// Only do this when in a level. Also do this after the hook, so that it can't overwrite this.
		cmd->latency = (leveltime & TICCMD_LATENCYMASK);
	}

	// Turning was removed from G_BuildTiccmd to prevent easy client hacking.
	// This brings back the camera prediction that was lost.
	void angle_prediction()
	{
		// Chasecam stops in these situations, so local cam should stop too.
		// Otherwise it'll jerk when it resumes.
		if (player()->playerstate == PST_DEAD)
		{
			return;
		}

		if (player()->mo != NULL && !P_MobjWasRemoved(player()->mo) && player()->mo->hitlag > 0)
		{
			return;
		}

		angle_t angleChange = 0;

		while (realtics > 0)
		{
			INT32& steering = localsteering[forplayer()];

			steering = K_UpdateSteeringValue(steering, cmd->turning);
			angleChange = K_GetKartTurnValue(player(), steering) << TICCMD_REDUCE;

			realtics--;
		}

#if 0
		// Left here in case it needs unsealing later. This tried to replicate an old localcam function, but this behavior was unpopular in tests.
		//if (player()->pflags & PF_DRIFTEND)
		{
			localangle[forplayer()] = player()->mo->angle;
		}
		else
#endif
		{
			localangle[viewnum] += angleChange;
		}
	}

	bool typing_input()
	{
		if (!menuactive && !chat_on && !CON_Ready())
		{
			return false;
		}

		cmd->flags |= TICCMD_TYPING;

		if (hu_keystrokes)
		{
			cmd->flags |= TICCMD_KEYSTROKE;
		}

		return true;
	}

	void toggle_freecam_input()
	{
		if (M_MenuButtonPressed(forplayer(), MBT_C))
		{
			P_ToggleDemoCamera();
		}
	}

	bool director_input()
	{
		if (demo.freecam || G_IsPartyLocal(displayplayers[forplayer()]) == true)
		{
			return false;
		}

		if (M_MenuButtonPressed(forplayer(), MBT_A))
		{
			G_AdjustView(ssplayer, 1, true);
			K_ToggleDirector(false);
		}

		if (M_MenuButtonPressed(forplayer(), MBT_X))
		{
			G_AdjustView(ssplayer, -1, true);
			K_ToggleDirector(false);
		}

		if (player()->spectator == true)
		{
			// duplication of fire
			if (G_PlayerInputDown(forplayer(), gc_item, 0))
			{
				cmd->buttons |= BT_ATTACK;
			}

			if (M_MenuButtonPressed(forplayer(), MBT_R))
			{
				K_ToggleDirector(true);
			}
		}

		toggle_freecam_input();

		return true;
	}

	bool spectator_analog_input()
	{
		if (!player()->spectator && !objectplacing && !demo.freecam)
		{
			return false;
		}

		if (G_PlayerInputDown(forplayer(), gc_accel, 0))
		{
			cmd->buttons |= BT_ACCELERATE;
		}

		if (G_PlayerInputDown(forplayer(), gc_brake, 0))
		{
			cmd->buttons |= BT_BRAKE;
		}

		if (G_PlayerInputDown(forplayer(), gc_lookback, 0))
		{
			cmd->aiming -= (joystickvector.yaxis * KART_FULLTURN) / JOYAXISRANGE;
		}
		else
		{
			if (joystickvector.yaxis < 0)
			{
				cmd->forwardmove += MAXPLMOVE;
			}

			if (joystickvector.yaxis > 0)
			{
				cmd->forwardmove -= MAXPLMOVE;
			}
		}

		return true;
	}

	void kart_analog_input()
	{
		// forward with key or button // SRB2kart - we use an accel/brake instead of forward/backward.
		INT32 value = G_PlayerInputAnalog(forplayer(), gc_accel, 0);
		if (value != 0)
		{
			cmd->buttons |= BT_ACCELERATE;
			cmd->forwardmove += ((value * MAXPLMOVE) / JOYAXISRANGE);
		}

		value = G_PlayerInputAnalog(forplayer(), gc_brake, 0);
		if (value != 0)
		{
			cmd->buttons |= BT_BRAKE;
			cmd->forwardmove -= ((value * MAXPLMOVE) / JOYAXISRANGE);
		}

		// But forward/backward IS used for aiming.
		if (joystickvector.yaxis != 0)
		{
			cmd->throwdir -= (joystickvector.yaxis * KART_FULLTURN) / JOYAXISRANGE;
		}
	}

	void analog_input()
	{
		joystickvector.xaxis = G_PlayerInputAnalog(forplayer(), gc_right, 0) - G_PlayerInputAnalog(forplayer(), gc_left, 0);
		joystickvector.yaxis = 0;
		handle_axis_deadzone();

		// For kart, I've turned the aim axis into a digital axis because we only
		// use it for aiming to throw items forward/backward and the vote screen
		// This mean that the turn axis will still be gradient but up/down will be 0
		// until the stick is pushed far enough
		joystickvector.yaxis = G_PlayerInputAnalog(forplayer(), gc_down, 0) - G_PlayerInputAnalog(forplayer(), gc_up, 0);

		if (encoremode)
		{
			joystickvector.xaxis = -joystickvector.xaxis;
		}

		if (joystickvector.xaxis != 0)
		{
			cmd->turning -= (joystickvector.xaxis * KART_FULLTURN) / JOYAXISRANGE;
		}

		if (spectator_analog_input())
		{
			return;
		}

		kart_analog_input();
	}

	void common_button_input()
	{
		auto map = [this](INT32 gamecontrol, UINT32 button)
		{
			if (G_PlayerInputDown(forplayer(), gamecontrol, 0))
			{
				cmd->buttons |= button;
			}
		};

		map(gc_drift, BT_DRIFT); // drift
		map(gc_spindash, BT_SPINDASHMASK); // C
		map(gc_item, BT_ATTACK); // fire

		map(gc_lookback, BT_LOOKBACK); // rear view
		map(gc_respawn, BT_RESPAWN | BT_EBRAKEMASK); // respawn
		map(gc_vote, BT_VOTE); // mp general function button

		// lua buttons a thru c
		map(gc_luaa, BT_LUAA);
		map(gc_luab, BT_LUAB);
		map(gc_luac, BT_LUAC);
	}

public:
	explicit TiccmdBuilder(ticcmd_t* cmd_, INT32 realtics_, UINT8 ssplayer_) :
		cmd(cmd_), realtics(realtics_), ssplayer(ssplayer_), viewnum(G_PartyPosition(g_localplayers[forplayer()]))
	{
		auto regular_input = [this]
		{
			analog_input();
			common_button_input();
		};

		if (demo.playback || demo.freecam || player()->spectator)
		{
			// freecam is controllable even while paused

			*cmd = {};

			if (!typing_input() && !director_input())
			{
				regular_input();

				if (demo.freecam)
				{
					toggle_freecam_input();
				}
			}

			return;
		}

		if (paused || P_AutoPause())
		{
			return;
		}

		*cmd = {}; // blank ticcmd

		if (gamestate == GS_LEVEL && player()->playerstate == PST_REBORN)
		{
			return;
		}

		// A human player can turn into a bot at the end of
		// a race, so the director controls have higher
		// priority.
		bool overlay = typing_input() || director_input();

		if (K_PlayerUsesBotMovement(player()))
		{
			// Bot ticcmd is generated by K_BuildBotTiccmd
			return;
		}

		if (!overlay)
		{
			regular_input();
		}

		cmd->angle = localangle[viewnum] >> TICCMD_REDUCE;

		hook();

		angle_prediction();
	}
};

}; // namespace

void G_BuildTiccmd(ticcmd_t *cmd, INT32 realtics, UINT8 ssplayer)
{
	TiccmdBuilder(cmd, realtics, ssplayer);
}