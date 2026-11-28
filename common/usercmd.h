/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
#pragma once
#if !defined(USERCMD_H)
#define USERCMD_H

// Project free-aim payload carried in the existing GoldSrc/Xash impact fields.
// The fields remain part of the 52-byte command; no wire-size change is required.
#define FREE_AIM_USERCMD_TAG 0x46524149
#define FREE_AIM_MAX_YAW 8.0f
#define FREE_AIM_MAX_PITCH 5.0f

typedef struct usercmd_s
{
	short		lerp_msec;	// Interpolation time on client
	byte		msec;		// Duration in ms of command
	vec3_t		viewangles;	// Command view angles

	// intended velocities
	float		forwardmove;	// Forward velocity
	float		sidemove;		// Sideways velocity
	float		upmove;		// Upward velocity
	byte		lightlevel;	// Light level at spot where we are standing.
	unsigned short	buttons;		// Attack and move buttons
	byte		impulse;		// Impulse command issued
	byte		weaponselect;	// Current weapon id

	// Experimental player impact stuff.
	int		impact_index;
	vec3_t		impact_position;
} usercmd_t;

#endif//USERCMD_H
