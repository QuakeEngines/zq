/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// cl_ents.c -- entity parsing and management

#include "quakedef.h"
#include "pmove.h"
#include "teamplay.h"


extern cvar_t	cl_predictPlayers;
extern cvar_t	cl_solidPlayers;

static struct predicted_player {
	int flags;
	qboolean active;
	vec3_t origin;	// predicted origin
} predicted_players[MAX_CLIENTS];

/*
=========================================================================

PACKET ENTITY PARSING / LINKING

=========================================================================
*/

/*
==================
CL_ParseDelta

Can go from either a baseline or a previous packet_entity
==================
*/
int	bitcounts[32];	/// just for protocol profiling
void CL_ParseDelta (entity_state_t *from, entity_state_t *to, int bits)
{
	int			i;

	// set everything to the state we are delta'ing from
	*to = *from;

	to->number = bits & 511;
	bits &= ~511;

	if (bits & U_MOREBITS)
	{	// read in the low order bits
		i = MSG_ReadByte ();
		bits |= i;
	}

	// count the bits for net profiling
	for (i=0 ; i<16 ; i++)
		if (bits&(1<<i))
			bitcounts[i]++;

	to->flags = bits;
	
	if (bits & U_MODEL)
		to->modelindex = MSG_ReadByte ();
		
	if (bits & U_FRAME)
		to->frame = MSG_ReadByte ();

	if (bits & U_COLORMAP)
		to->colormap = MSG_ReadByte();

	if (bits & U_SKIN)
		to->skinnum = MSG_ReadByte();

	if (bits & U_EFFECTS)
		to->effects = MSG_ReadByte();

	if (bits & U_ORIGIN1)
		to->origin[0] = MSG_ReadCoord ();
		
	if (bits & U_ANGLE1)
		to->angles[0] = MSG_ReadAngle();

	if (bits & U_ORIGIN2)
		to->origin[1] = MSG_ReadCoord ();
		
	if (bits & U_ANGLE2)
		to->angles[1] = MSG_ReadAngle();

	if (bits & U_ORIGIN3)
		to->origin[2] = MSG_ReadCoord ();
		
	if (bits & U_ANGLE3)
		to->angles[2] = MSG_ReadAngle();

	if (bits & U_SOLID)
	{
		// FIXME
	}
}


/*
=================
FlushEntityPacket
=================
*/
void FlushEntityPacket (void)
{
	int			word;
	entity_state_t	olde, newe;

	Com_DPrintf ("FlushEntityPacket\n");

	memset (&olde, 0, sizeof(olde));

	cl.delta_sequence = 0;
	cl.frames[cls.netchan.incoming_sequence&UPDATE_MASK].invalid = true;

	// read it all, but ignore it
	while (1)
	{
		word = (unsigned short)MSG_ReadShort ();
		if (msg_badread)
		{	// something didn't parse right...
			Host_Error ("msg_badread in packetentities");
			return;
		}

		if (!word)
			break;	// done

		CL_ParseDelta (&olde, &newe, word);
	}
}

entity_state_t *CL_GetBaseline (int number)
{
	return &cl_entities[number].baseline;
}

// bump lastframe and copy current state to previous
static void UpdateEntities (void)
{
	int		i;
	packet_entities_t *pack;
	entity_state_t *ent;
	centity_t	*cent;

	assert (cl.validsequence);

	pack = &cl.frames[cl.validsequence & UPDATE_MASK].packet_entities;

	for (i = 0; i < pack->num_entities; i++) {
		ent = &pack->entities[i];
		cent = &cl_entities[ent->number];

		cent->prevframe = cent->lastframe;
		cent->lastframe = cl_entframecount;

		if (cent->prevframe == cl_oldentframecount) {
			// move along, move along
			cent->previous = cent->current;
		} else {
			// not in previous message
			cent->previous = *ent;
			VectorCopy (cent->current.origin, cent->lerp_origin);
		}

		cent->current = *ent;
	}
}

/*
==================
CL_ParsePacketEntities

An svc_packetentities has just been parsed, deal with the
rest of the data stream.
==================
*/
void CL_ParsePacketEntities (qboolean delta)
{
	int			oldpacket, newpacket;
	packet_entities_t	*oldp, *newp, dummy;
	int			oldindex, newindex;
	int			word, newnum, oldnum;
	qboolean	full;
	byte		from;

	newpacket = cls.netchan.incoming_sequence&UPDATE_MASK;
	newp = &cl.frames[newpacket].packet_entities;
	cl.frames[newpacket].invalid = false;

	if (delta)
	{
		from = MSG_ReadByte ();

		oldpacket = cl.frames[newpacket].delta_sequence;

		if (cls.netchan.outgoing_sequence - cls.netchan.incoming_sequence >= UPDATE_BACKUP-1)
		{	// there are no valid frames left, so drop it
			FlushEntityPacket ();
			cl.validsequence = 0;
			return;
		}

		if ( (from&UPDATE_MASK) != (oldpacket&UPDATE_MASK) ) {
			Com_DPrintf ("WARNING: from mismatch\n");
			FlushEntityPacket ();
			cl.validsequence = 0;
			return;
		}

		if (cls.netchan.outgoing_sequence - oldpacket >= UPDATE_BACKUP-1)
		{	// we can't use this, it is too old
			FlushEntityPacket ();
			// don't clear cl.validsequence, so that frames can
			// still be rendered; it is possible that a fresh packet will
			// be received before (outgoing_sequence - incoming_sequence)
			// exceeds UPDATE_BACKUP-1
			return;
		}

		oldp = &cl.frames[oldpacket&UPDATE_MASK].packet_entities;
		full = false;
	}
	else
	{	// this is a full update that we can start delta compressing from now
		oldp = &dummy;
		dummy.num_entities = 0;
		full = true;
	}

	cl.oldvalidsequence = cl.validsequence;
	cl.validsequence = cls.netchan.incoming_sequence;
	cl.delta_sequence = cl.validsequence;

	oldindex = 0;
	newindex = 0;
	newp->num_entities = 0;

	while (1)
	{
		word = (unsigned short)MSG_ReadShort ();
		if (msg_badread)
		{	// something didn't parse right...
			Host_Error ("msg_badread in packetentities");
			return;
		}

		if (!word)
		{
			while (oldindex < oldp->num_entities)
			{	// copy all the rest of the entities from the old packet
				if (newindex >= MAX_PACKET_ENTITIES)
					Host_Error ("CL_ParsePacketEntities: newindex == MAX_PACKET_ENTITIES");
				newp->entities[newindex] = oldp->entities[oldindex];
				newindex++;
				oldindex++;
			}
			break;
		}
		newnum = word&511;
		oldnum = oldindex >= oldp->num_entities ? 9999 : oldp->entities[oldindex].number;

		while (newnum > oldnum)
		{
			if (full)
			{
				Com_Printf ("WARNING: oldcopy on full update");
				FlushEntityPacket ();
				cl.validsequence = 0;	// can't render a frame
				return;
			}

			// copy one of the old entities over to the new packet unchanged
			if (newindex >= MAX_PACKET_ENTITIES)
				Host_Error ("CL_ParsePacketEntities: newindex == MAX_PACKET_ENTITIES");
			newp->entities[newindex] = oldp->entities[oldindex];
			newindex++;
			oldindex++;
			oldnum = oldindex >= oldp->num_entities ? 9999 : oldp->entities[oldindex].number;
		}

		if (newnum < oldnum)
		{	// new from baseline

			if (word & U_REMOVE)
			{
				if (full)
				{
					Com_Printf ("WARNING: U_REMOVE on full update\n");
					FlushEntityPacket ();
					cl.validsequence = 0;	// can't render a frame
					return;
				}
				continue;
			}
			if (newindex >= MAX_PACKET_ENTITIES)
				Host_Error ("CL_ParsePacketEntities: newindex == MAX_PACKET_ENTITIES");
			CL_ParseDelta (&cl_entities[newnum].baseline, &newp->entities[newindex], word);
			newindex++;
			continue;
		}

		if (newnum == oldnum)
		{	// delta from previous
			if (full)
			{
				cl.validsequence = 0;
				cl.delta_sequence = 0;
				Com_Printf ("WARNING: delta on full update");
			}
			if (word & U_REMOVE)
			{
				oldindex++;
				continue;
			}

			CL_ParseDelta (&oldp->entities[oldindex], &newp->entities[newindex], word);
			newindex++;
			oldindex++;
		}
	}

	newp->num_entities = newindex;

	cl_oldentframecount = cl_entframecount;
	cl_entframecount++;
	UpdateEntities ();

	if (cls.demorecording) {
		// write uncompressed packetentities to the demo
		MSG_EmitPacketEntities (NULL, -1, newp, &cls.demomessage, CL_GetBaseline);
		cls.demomessage_skipwrite = true;
	}

	// we can now render a frame
	if (cls.state == ca_onserver)
	{	
		extern qboolean host_skipframe;
		
		// first update is the final signon stage
		cls.state = ca_active;
		if (cls.demoplayback)
			host_skipframe = true;

		if (!cls.demoplayback)
			VID_SetCaption (va(PROGRAM ": %s", cls.servername));

		Con_ClearNotify ();
		TP_ExecTrigger ("f_spawn");

		SCR_EndLoadingPlaque ();
	}
}


extern int	cl_playerindex; 
extern int	cl_h_playerindex, cl_gib1index, cl_gib2index, cl_gib3index;
extern int	cl_rocketindex, cl_grenadeindex;

/*
===============
CL_LinkPacketEntities

===============
*/
void CL_LinkPacketEntities (void)
{
	entity_t			ent;
	centity_t			*cent;
	packet_entities_t	*pack;
	entity_state_t		*state;
	float				f;
	model_t				*model;
	vec3_t				old_origin;
	float				autorotate, flicker;
	int					i;
	int					pnum;

	pack = &cl.frames[cl.validsequence&UPDATE_MASK].packet_entities;

	autorotate = anglemod (100*cl.time);

	memset (&ent, 0, sizeof(ent));

	f = 1.0f;		// FIXME: no interpolation right now


	for (pnum=0 ; pnum<pack->num_entities ; pnum++)
	{
		state = &pack->entities[pnum];
		cent = &cl_entities[state->number];

		assert(cent->lastframe != cl_entframecount);
		assert(!memcmp(state, &cent->current, sizeof(*state)));

		// control powerup glow for bots
		if (state->modelindex != cl_playerindex || r_powerupglow.value)
		{
			flicker = r_lightflicker.value ? (rand() & 31) : 10;
			// spawn light flashes, even ones coming from invisible objects
			if ((state->effects & (EF_BLUE | EF_RED)) == (EF_BLUE | EF_RED))
				CL_NewDlight (state->number, state->origin, 200 + flicker, 0.1, lt_redblue);
			else if (state->effects & EF_BLUE)
				CL_NewDlight (state->number, state->origin, 200 + flicker, 0.1, lt_blue);
			else if (state->effects & EF_RED)
				CL_NewDlight (state->number, state->origin, 200 + flicker, 0.1, lt_red);
			else if (state->effects & EF_BRIGHTLIGHT) {
				vec3_t	tmp;
				VectorCopy (state->origin, tmp);
				tmp[2] += 16;
				CL_NewDlight (state->number, tmp, 400 + flicker, 0.1, lt_default);
			} else if (state->effects & EF_DIMLIGHT)
				CL_NewDlight (state->number, state->origin, 200 + flicker, 0.1, lt_default);
		}

		// if set to invisible, skip
		if (!state->modelindex)
			continue;

		if (cl_deadbodyfilter.value && state->modelindex == cl_playerindex
			&& ( (i=state->frame)==49 || i==60 || i==69 || i==84 || i==93 || i==102) )
			continue;

		if (cl_gibfilter.value && (state->modelindex == cl_h_playerindex
			|| state->modelindex == cl_gib1index || state->modelindex == cl_gib2index || state->modelindex == cl_gib3index))
			continue;

		ent.model = model = cl.model_precache[state->modelindex];
		if (!model)
			Host_Error ("CL_LinkPacketEntities: bad modelindex");

		if (cl_r2g.value && cl_grenadeindex != -1)
			if (state->modelindex == cl_rocketindex)
				ent.model = cl.model_precache[cl_grenadeindex];

		// set colormap
		if (state->colormap && (state->colormap < MAX_CLIENTS) 
			&& ent.model->modhint == MOD_PLAYER)
		{
			ent.colormap = cl.players[state->colormap-1].translations;
			ent.scoreboard = &cl.players[state->colormap-1];
		}
		else
		{
			ent.colormap = vid.colormap;
			ent.scoreboard = NULL;
		}

		// set skin
		ent.skinnum = state->skinnum;
		
		// set frame
		ent.frame = state->frame;

		// rotate binary objects locally
		if (model->flags & EF_ROTATE)
		{
			ent.angles[0] = 0;
			ent.angles[1] = autorotate;
			ent.angles[2] = 0;
		}
		else
		{
			float	a1, a2;

			for (i=0 ; i<3 ; i++)
			{
				a1 = cent->current.angles[i];
				a2 = cent->previous.angles[i];
				if (a1 - a2 > 180)
					a1 -= 360;
				if (a1 - a2 < -180)
					a1 += 360;
				ent.angles[i] = a2 + f * (a1 - a2);
			}
		}

		// calculate origin
		for (i=0 ; i<3 ; i++)
			ent.origin[i] = cent->previous.origin[i] + 
				f * (cent->current.origin[i] - cent->previous.origin[i]);

		// add automatic particle trails
		if (model->flags & ~EF_ROTATE)
		{
			VectorCopy (cent->lerp_origin, old_origin);

			for (i=0 ; i<3 ; i++)
				if (abs(old_origin[i] - ent.origin[i]) > 128)
				{	// no trail if too far
					VectorCopy (ent.origin, old_origin);
					break;
				}

			if (model->flags & EF_ROCKET)
			{
				if (r_rockettrail.value) {
					if (r_rockettrail.value == 2)
						CL_GrenadeTrail (old_origin, ent.origin);
					else
						CL_RocketTrail (old_origin, ent.origin);
				}

				if (r_rocketlight.value)
					CL_NewDlight (state->number, ent.origin, 200, 0.1, lt_rocket);
			}
			else if (model->flags & EF_GRENADE && r_grenadetrail.value)
				CL_GrenadeTrail (old_origin, ent.origin);
			else if (model->flags & EF_GIB)
				CL_BloodTrail (old_origin, ent.origin);
			else if (model->flags & EF_ZOMGIB)
				CL_SlightBloodTrail (old_origin, ent.origin);
			else if (model->flags & EF_TRACER)
				CL_TracerTrail (old_origin, ent.origin, 52);
			else if (model->flags & EF_TRACER2)
				CL_TracerTrail (old_origin, ent.origin, 230);
			else if (model->flags & EF_TRACER3)
				CL_VoorTrail (old_origin, ent.origin);
		}

		VectorCopy (ent.origin, cent->lerp_origin);
		V_AddEntity (&ent);
	}
}


/*
=========================================================================

PROJECTILE PARSING / LINKING

=========================================================================
*/

typedef struct
{
	int		modelindex;
	vec3_t	origin;
	vec3_t	angles;
} projectile_t;

#define	MAX_PROJECTILES	32
projectile_t	cl_projectiles[MAX_PROJECTILES];
int				cl_num_projectiles;

extern int cl_spikeindex;

void CL_ClearProjectiles (void)
{
	cl_num_projectiles = 0;
}

/*
=====================
CL_ParseProjectiles

Nails are passed as efficient temporary entities
=====================
*/
void CL_ParseProjectiles (void)
{
	int		i, c, j;
	byte	bits[6];
	projectile_t	*pr;

	c = MSG_ReadByte ();
	for (i=0 ; i<c ; i++)
	{
		for (j=0 ; j<6 ; j++)
			bits[j] = MSG_ReadByte ();

		if (cl_num_projectiles == MAX_PROJECTILES)
			continue;

		pr = &cl_projectiles[cl_num_projectiles];
		cl_num_projectiles++;

		pr->modelindex = cl_spikeindex;
		pr->origin[0] = ( ( bits[0] + ((bits[1]&15)<<8) ) <<1) - 4096;
		pr->origin[1] = ( ( (bits[1]>>4) + (bits[2]<<4) ) <<1) - 4096;
		pr->origin[2] = ( ( bits[3] + ((bits[4]&15)<<8) ) <<1) - 4096;
		pr->angles[0] = 360*(bits[4]>>4)/16;
		pr->angles[1] = 360*bits[5]/256;
	}
}

/*
=============
CL_LinkProjectiles

=============
*/
void CL_LinkProjectiles (void)
{
	int		i;
	projectile_t	*pr;
	entity_t		ent;

	memset (&ent, 0, sizeof(entity_t));
	ent.colormap = vid.colormap;

	for (i=0, pr=cl_projectiles ; i<cl_num_projectiles ; i++, pr++)
	{
		if (pr->modelindex < 1)
			continue;

		ent.model = cl.model_precache[pr->modelindex];
		VectorCopy (pr->origin, ent.origin);
		VectorCopy (pr->angles, ent.angles);

		V_AddEntity (&ent);
	}
}

//========================================

extern	int		cl_spikeindex, cl_playerindex, cl_flagindex;

/*
===================
CL_ParsePlayerinfo
===================
*/
extern int parsecountmod;
extern double parsecounttime;
void CL_ParsePlayerinfo (void)
{
	int			msec;
	int			flags;
	player_info_t	*info;
	player_state_t	*state;
	int			num;
	int			i;

	num = MSG_ReadByte ();
	if (num >= MAX_CLIENTS)
		Host_Error ("CL_ParsePlayerinfo: bad num");

	info = &cl.players[num];

	state = &cl.frames[parsecountmod].playerstate[num];

	flags = state->flags = MSG_ReadShort ();

	state->messagenum = cl.parsecount;
	state->origin[0] = MSG_ReadCoord ();
	state->origin[1] = MSG_ReadCoord ();
	state->origin[2] = MSG_ReadCoord ();

	state->frame = MSG_ReadByte ();

	// the other player's last move was likely some time
	// before the packet was sent out, so accurately track
	// the exact time it was valid at
	if (flags & PF_MSEC)
	{
		msec = MSG_ReadByte ();
		state->state_time = parsecounttime - msec*0.001;
	}
	else
		state->state_time = parsecounttime;

	if (flags & PF_COMMAND)
		MSG_ReadDeltaUsercmd (&nullcmd, &state->command, cl.protocol_26);

	for (i=0 ; i<3 ; i++)
	{
		if (flags & (PF_VELOCITY1<<i) )
			state->velocity[i] = MSG_ReadShort();
		else
			state->velocity[i] = 0;
	}
	if (flags & PF_MODEL)
		state->modelindex = MSG_ReadByte ();
	else
		state->modelindex = cl_playerindex;

	if (flags & PF_SKINNUM)
		state->skinnum = MSG_ReadByte ();
	else
		state->skinnum = 0;

	if (flags & PF_EFFECTS)
		state->effects = MSG_ReadByte ();
	else
		state->effects = 0;

	if (flags & PF_WEAPONFRAME)
		state->weaponframe = MSG_ReadByte ();
	else
		state->weaponframe = 0;

	if (cl.z_ext & Z_EXT_PM_TYPE)
	{
		int pm_code = (flags >> PF_PMC_SHIFT) & PF_PMC_MASK;

		if (pm_code == PMC_NORMAL || pm_code == PMC_NORMAL_JUMP_HELD) {
			state->pm_type = PM_NORMAL;
			state->jump_held = (pm_code == PMC_NORMAL_JUMP_HELD);
		}
		else if (pm_code == PMC_OLD_SPECTATOR)
			state->pm_type = PM_OLD_SPECTATOR;
		else {
			if (cl.z_ext & Z_EXT_PM_TYPE_NEW) {
				if (pm_code == PMC_SPECTATOR)
					state->pm_type = PM_SPECTATOR;
				else if (pm_code == PMC_FLY)
					state->pm_type = PM_FLY;
				else if (pm_code == PMC_NONE)
					state->pm_type = PM_NONE;
				else if (pm_code == PMC_FREEZE)
					state->pm_type = PM_FREEZE;
				else {
					// future extension?
					goto guess_pm_type;
				}
			}
			else {
				// future extension?
				goto guess_pm_type;
			}
		}
	}
	else
	{
guess_pm_type:
		if (cl.players[num].spectator)
			state->pm_type = PM_OLD_SPECTATOR;
		else if (flags & PF_DEAD)
			state->pm_type = PM_DEAD;
		else
			state->pm_type = PM_NORMAL;
	}

	VectorCopy (state->command.angles, state->viewangles);
}


/*
================
CL_AddFlagModels

Called when the CTF flags are set
================
*/
void CL_AddFlagModels (entity_t *ent, int team)
{
	int		i;
	float	f;
	vec3_t	v_forward, v_right;
	entity_t	newent;

	if (cl_flagindex == -1)
		return;

	f = 14;
	if (ent->frame >= 29 && ent->frame <= 40) {
		if (ent->frame >= 29 && ent->frame <= 34) { //axpain
			if      (ent->frame == 29) f = f + 2; 
			else if (ent->frame == 30) f = f + 8;
			else if (ent->frame == 31) f = f + 12;
			else if (ent->frame == 32) f = f + 11;
			else if (ent->frame == 33) f = f + 10;
			else if (ent->frame == 34) f = f + 4;
		} else if (ent->frame >= 35 && ent->frame <= 40) { // pain
			if      (ent->frame == 35) f = f + 2; 
			else if (ent->frame == 36) f = f + 10;
			else if (ent->frame == 37) f = f + 10;
			else if (ent->frame == 38) f = f + 8;
			else if (ent->frame == 39) f = f + 4;
			else if (ent->frame == 40) f = f + 2;
		}
	} else if (ent->frame >= 103 && ent->frame <= 118) {
		if      (ent->frame >= 103 && ent->frame <= 104) f = f + 6;  //nailattack
		else if (ent->frame >= 105 && ent->frame <= 106) f = f + 6;  //light 
		else if (ent->frame >= 107 && ent->frame <= 112) f = f + 7;  //rocketattack
		else if (ent->frame >= 112 && ent->frame <= 118) f = f + 7;  //shotattack
	}

	memset (&newent, 0, sizeof(entity_t));

	newent.model = cl.model_precache[cl_flagindex];
	newent.skinnum = team;
	newent.colormap = vid.colormap;

	AngleVectors (ent->angles, v_forward, v_right, NULL);
	v_forward[2] = -v_forward[2]; // reverse z component
	for (i=0 ; i<3 ; i++)
		newent.origin[i] = ent->origin[i] - f*v_forward[i] + 22*v_right[i];
	newent.origin[2] -= 16;

	VectorCopy (ent->angles, newent.angles);
	newent.angles[2] -= 45;

	V_AddEntity (&newent);
}

/*
=============
CL_LinkPlayers

Create visible entities in the correct position
for all current players
=============
*/
void CL_LinkPlayers (void)
{
	int				i, j;
	player_info_t	*info;
	player_state_t	*state;
	player_state_t	exact;
	double			playertime;
	entity_t		ent;
	centity_t		*cent;
	int				msec;
	frame_t			*frame;
	int				oldphysent;
	vec3_t			org;
	float			flicker;

	playertime = cls.realtime - cls.latency + 0.02;
	if (playertime > cls.realtime)
		playertime = cls.realtime;

	frame = &cl.frames[cl.parsecount&UPDATE_MASK];

	memset (&ent, 0, sizeof(entity_t));

	for (j=0, info=cl.players, state=frame->playerstate ; j < MAX_CLIENTS 
		; j++, info++, state++)
	{
		if (state->messagenum != cl.parsecount)
			continue;	// not present this frame

		// spawn light flashes, even ones coming from invisible objects
		if (r_powerupglow.value && !(r_powerupglow.value == 2 && j == cl.viewplayernum))
		{
			if (j == cl.playernum) {
				VectorCopy (cl.simorg, org);
			} else
				VectorCopy (state->origin, org);

			flicker = r_lightflicker.value ? (rand() & 31) : 10;

			if ((state->effects & (EF_BLUE | EF_RED)) == (EF_BLUE | EF_RED))
				CL_NewDlight (j+1, org, 200 + flicker, 0.1, lt_redblue);
			else if (state->effects & EF_BLUE)
				CL_NewDlight (j+1, org, 200 + flicker, 0.1, lt_blue);
			else if (state->effects & EF_RED)
				CL_NewDlight (j+1, org, 200 + flicker, 0.1, lt_red);
			else if (state->effects & EF_BRIGHTLIGHT) {
				vec3_t	tmp;
				VectorCopy (org, tmp);
				tmp[2] += 16;
				CL_NewDlight (j+1, tmp, 400 + flicker, 0.1, lt_default);
			}
			else if (state->effects & EF_DIMLIGHT)
				CL_NewDlight (j+1, org, 200 + flicker, 0.1, lt_default);
		}

		if (!state->modelindex)
			continue;

		cent = &cl_entities[j+1];
		cent->previous = cent->current;
		VectorCopy (state->origin, cent->current.origin);

		// the player object never gets added
		if (j == cl.playernum)
			continue;

		if (cl_deadbodyfilter.value && state->modelindex == cl_playerindex
			&& ( (i=state->frame)==49 || i==60 || i==69 || i==84 || i==93 || i==102) )
			continue;
		
		if (!Cam_DrawPlayer(j))
			continue;

		ent.model = cl.model_precache[state->modelindex];
		if (!ent.model)
			Host_Error ("CL_LinkPlayers: bad modelindex");
		ent.skinnum = state->skinnum;
		ent.frame = state->frame;
		ent.colormap = info->translations;
		if (state->modelindex == cl_playerindex)
			ent.scoreboard = info;		// use custom skin
		else
			ent.scoreboard = NULL;

		//
		// angles
		//
		ent.angles[PITCH] = -state->viewangles[PITCH]/3;
		ent.angles[YAW] = state->viewangles[YAW];
		ent.angles[ROLL] = 0;
		ent.angles[ROLL] = V_CalcRoll (ent.angles, state->velocity)*4;

		// only predict half the move to minimize overruns
		msec = 500 * (playertime - state->state_time);
		if (msec <= 0 || !cl_predictPlayers.value)
		{
			VectorCopy (state->origin, ent.origin);
		}
		else
		{
			// predict players movement
			if (msec > 255)
				msec = 255;
			state->command.msec = msec;

			oldphysent = pmove.numphysent;
			CL_SetSolidPlayers (j);
			CL_PredictUsercmd (state, &exact, &state->command);
			pmove.numphysent = oldphysent;
			VectorCopy (exact.origin, ent.origin);
		}

		if (state->effects & EF_FLAG1)
			CL_AddFlagModels (&ent, 0);
		else if (state->effects & EF_FLAG2)
			CL_AddFlagModels (&ent, 1);

		VectorCopy (ent.origin, cent->lerp_origin);
		V_AddEntity (&ent);
	}
}

//======================================================================

/*
===============
CL_SetSolid

Builds all the pmove physents for the current frame
===============
*/
void CL_SetSolidEntities (void)
{
	int		i;
	frame_t	*frame;
	packet_entities_t	*pak;
	entity_state_t		*state;

	pmove.physents[0].model = cl.worldmodel;
	VectorClear (pmove.physents[0].origin);
	pmove.physents[0].info = 0;
	pmove.numphysent = 1;

	frame = &cl.frames[parsecountmod];
	pak = &frame->packet_entities;

	for (i=0 ; i<pak->num_entities ; i++)
	{
		state = &pak->entities[i];

		if (!state->modelindex)
			continue;
		if (!cl.model_precache[state->modelindex])
			continue;
		if (cl.model_precache[state->modelindex]->hulls[1].firstclipnode)
		{
			pmove.physents[pmove.numphysent].model = cl.model_precache[state->modelindex];
			VectorCopy (state->origin, pmove.physents[pmove.numphysent].origin);
			pmove.numphysent++;
		}
	}

}

/*
===
Calculate the new position of players, without other player clipping

We do this to set up real player prediction.
Players are predicted twice, first without clipping other players,
then with clipping against them.
This sets up the first phase.
===
*/
void CL_SetUpPlayerPrediction(qboolean dopred)
{
	int				j;
	player_state_t	*state;
	player_state_t	exact;
	double			playertime;
	int				msec;
	frame_t			*frame;
	struct predicted_player *pplayer;

	playertime = cls.realtime - cls.latency + 0.02;
	if (playertime > cls.realtime)
		playertime = cls.realtime;

	frame = &cl.frames[cl.parsecount&UPDATE_MASK];

	for (j=0, pplayer = predicted_players, state=frame->playerstate; 
		j < MAX_CLIENTS;
		j++, pplayer++, state++) {

		pplayer->active = false;

		if (state->messagenum != cl.parsecount)
			continue;	// not present this frame

		if (!state->modelindex)
			continue;

		pplayer->active = true;
		pplayer->flags = state->flags;

		// note that the local player is special, since he moves locally
		// we use his last predicted postition
		if (j == cl.playernum) {
			VectorCopy(cl.frames[cls.netchan.outgoing_sequence&UPDATE_MASK].playerstate[cl.playernum].origin,
				pplayer->origin);
		} else {
			// only predict half the move to minimize overruns
			msec = 500*(playertime - state->state_time);
			if (msec <= 0 || !cl_predictPlayers.value || !dopred)
			{
				VectorCopy (state->origin, pplayer->origin);
			}
			else
			{
				// predict players movement
				if (msec > 255)
					msec = 255;
				state->command.msec = msec;

				CL_PredictUsercmd (state, &exact, &state->command);
				VectorCopy (exact.origin, pplayer->origin);
			}
		}
	}
}

/*
===============
CL_SetSolid

Builds all the pmove physents for the current frame
Note that CL_SetUpPlayerPrediction() must be called first!
pmove must be setup with world and solid entity hulls before calling
(via CL_PredictMove)
===============
*/
void CL_SetSolidPlayers (int playernum)
{
	int		j;
	extern	vec3_t	player_mins;
	extern	vec3_t	player_maxs;
	struct predicted_player *pplayer;
	physent_t *pent;

	if (!cl_solidPlayers.value)
		return;

	pent = pmove.physents + pmove.numphysent;

	for (j=0, pplayer = predicted_players; j < MAX_CLIENTS;	j++, pplayer++) {

		if (!pplayer->active)
			continue;	// not present this frame

		// the player object never gets added
		if (j == playernum)
			continue;

		if (pplayer->flags & PF_DEAD)
			continue; // dead players aren't solid

		pent->model = 0;
		VectorCopy(pplayer->origin, pent->origin);
		VectorCopy(player_mins, pent->mins);
		VectorCopy(player_maxs, pent->maxs);
		pmove.numphysent++;
		pent++;
	}
}


/*
===============
CL_EmitEntities

Builds the visedicts array for cl.time

Made up of: clients, packet_entities, nails, and tents
===============
*/
void CL_EmitEntities (void)
{
	if (cls.state != ca_active)
		return;
	if (!cl.validsequence && !cls.nqdemoplayback)
		return;

	V_ClearScene ();

	if (cls.nqdemoplayback)
		NQD_LinkEntities ();
	else {
		CL_LinkPlayers ();
		CL_LinkPacketEntities ();
		CL_LinkProjectiles ();
	}
	CL_LinkParticles ();

	CL_UpdateTEnts ();
}

