/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the included (GNU.txt) GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#include "winquake.h"
#include "pmove.h"
#include "teamplay.h"


#define dem_cmd		0
#define dem_read	1
#define dem_set		2


void Cam_TryLock (void);
void CL_FinishTimeDemo (void);

// .qwz playback
static qboolean	qwz_unpacking = false;

#ifdef _WIN32
static qboolean	qwz_playback = false;
static HANDLE	hQizmoProcess = NULL;
static char tempqwd_name[256] = ""; // this file must be deleted
									// after playback is finished
void CheckQizmoCompletion ();
void StopQWZPlayback ();
#endif

/*
==============================================================================

DEMO CODE

When a demo is playing back, all NET_SendMessages are skipped, and
NET_GetMessages are read from the demo file.

Whenever cl.time gets past the last received message, another message is
read from the demo file.
==============================================================================
*/

/*
==============
CL_StopPlayback

Called when a demo file runs out, or the user starts a game
==============
*/
void CL_StopPlayback (void)
{
	if (!cls.demoplayback)
		return;

	if (cls.demofile)
		fclose (cls.demofile);
	cls.demofile = NULL;
	cls.demoplayback = false;

#ifdef _WIN32
	if (qwz_playback)
		StopQWZPlayback ();
#endif

	if (cls.timedemo)
		CL_FinishTimeDemo ();
}

/*
====================
CL_WriteDemoCmd

Writes the current user cmd
====================
*/
void CL_WriteDemoCmd (usercmd_t *pcmd)
{
	int		i;
	float	fl, t[3];
	byte	c;
	usercmd_t cmd;

//Com_Printf ("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	fl = LittleFloat((float)realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_cmd;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	// correct for byte order, bytes don't matter
	cmd = *pcmd;

	for (i = 0; i < 3; i++)
		cmd.angles[i] = LittleFloat(cmd.angles[i]);
	cmd.forwardmove = LittleShort(cmd.forwardmove);
	cmd.sidemove    = LittleShort(cmd.sidemove);
	cmd.upmove      = LittleShort(cmd.upmove);

	fwrite(&cmd, sizeof(cmd), 1, cls.demofile);

	t[0] = LittleFloat (cl.viewangles[0]);
	t[1] = LittleFloat (cl.viewangles[1]);
	t[2] = LittleFloat (cl.viewangles[2]);
	fwrite (t, 12, 1, cls.demofile);

	fflush (cls.demofile);
}

/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length and view angles
====================
*/
void CL_WriteDemoMessage (sizebuf_t *msg)
{
	int		len;
	float	fl;
	byte	c;

//Com_Printf ("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	if (!cls.demorecording)
		return;

	fl = LittleFloat((float)realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_read;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	len = LittleLong (msg->cursize);
	fwrite (&len, 4, 1, cls.demofile);
	fwrite (msg->data, msg->cursize, 1, cls.demofile);

	fflush (cls.demofile);
}

/*
====================
CL_GetDemoMessage

  FIXME...
====================
*/
qboolean CL_GetDemoMessage (void)
{
	int		r, i, j;
	float	demotime;
	byte	c;
	usercmd_t *pcmd;

	if (qwz_unpacking)
		return 0;

	if (cl.paused & 2)
		return 0;

	// read the time from the packet
	fread(&demotime, sizeof(demotime), 1, cls.demofile);
	demotime = LittleFloat(demotime);

// decide if it is time to grab the next message		
	if (cls.timedemo) {
		if (cls.td_lastframe < 0)
			cls.td_lastframe = demotime;
		else if (demotime > cls.td_lastframe) {
			cls.td_lastframe = demotime;
			// rewind back to time
			fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
					SEEK_SET);
			return 0;		// already read this frame's message
		}
		if (!cls.td_starttime && cls.state == ca_active) {
			cls.td_starttime = Sys_DoubleTime();
			cls.td_startframe = cls.framecount;
		}
		realtime = demotime; // warp
	} else if (!(cl.paused & 1) && cls.state >= ca_active) {	// always grab until active
		if (realtime + 1.0 < demotime) {
			// too far back
			realtime = demotime - 1.0;
			// rewind back to time
			fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
					SEEK_SET);
			return 0;
		} else if (realtime < demotime) {
			// rewind back to time
			fseek(cls.demofile, ftell(cls.demofile) - sizeof(demotime),
					SEEK_SET);
			return 0;		// don't need another message yet
		}
	} else
		realtime = demotime; // we're warping

	if (cls.state < ca_demostart)
		Host_Error ("CL_GetDemoMessage: cls.state != ca_active");
	
	// get the msg type
	fread (&c, sizeof(c), 1, cls.demofile);
	
	switch (c) {
	case dem_cmd :
		// user sent input
		i = cls.netchan.outgoing_sequence & UPDATE_MASK;
		pcmd = &cl.frames[i].cmd;
		r = fread (pcmd, sizeof(*pcmd), 1, cls.demofile);
		if (r != 1)
		{
			CL_Disconnect ();
			return 0;
		}
		// byte order stuff
		for (j = 0; j < 3; j++)
			pcmd->angles[j] = LittleFloat(pcmd->angles[j]);
		pcmd->forwardmove = LittleShort(pcmd->forwardmove);
		pcmd->sidemove    = LittleShort(pcmd->sidemove);
		pcmd->upmove      = LittleShort(pcmd->upmove);
		cl.frames[i].senttime = demotime;
		cl.frames[i].receivedtime = -1;		// we haven't gotten a reply yet
		cls.netchan.outgoing_sequence++;

		fread (cl.viewangles, 12, 1, cls.demofile);
		for (i = 0; i < 3; i++)
			cl.viewangles[i] = LittleFloat (cl.viewangles[i]);
		if (cl.spectator)
			Cam_TryLock ();
		break;

	case dem_read:
		// get the next message
		fread (&net_message.cursize, 4, 1, cls.demofile);
		net_message.cursize = LittleLong (net_message.cursize);
	//Com_Printf ("read: %ld bytes\n", net_message.cursize);
		if (net_message.cursize > MAX_MSGLEN + 8)
			Host_Error ("Demo message > MAX_MSGLEN");
		r = fread (net_message.data, net_message.cursize, 1, cls.demofile);
		if (r != 1)
		{
			CL_Disconnect ();
			return 0;
		}
		break;

	case dem_set:
		fread (&i, 4, 1, cls.demofile);
		cls.netchan.outgoing_sequence = LittleLong(i);
		fread (&i, 4, 1, cls.demofile);
		cls.netchan.incoming_sequence = LittleLong(i);
		break;

	default:
		Com_Printf ("Corrupted demo.\n");
		CL_Disconnect ();
		return 0;
	}

	return 1;
}

/*
====================
CL_GetMessage

Handles recording and playback of demos, on top of NET_ code
====================
*/
qboolean CL_GetMessage (void)
{
#ifdef _WIN32
	CheckQizmoCompletion ();
#endif

	if (cls.demoplayback)
		return CL_GetDemoMessage ();

	if (!NET_GetPacket(NS_CLIENT))
		return false;

	CL_WriteDemoMessage (&net_message);
	
	return true;
}


/*
====================
CL_Stop_f

stop recording a demo
====================
*/
void CL_Stop_f (void)
{
	if (!cls.demorecording)
	{
		Com_Printf ("Not recording a demo.\n");
		return;
	}

// write a disconnect message to the demo file
	SZ_Clear (&net_message);
	MSG_WriteLong (&net_message, -1);	// -1 sequence means out of band
	MSG_WriteByte (&net_message, svc_disconnect);
	MSG_WriteString (&net_message, "EndOfDemo");
	CL_WriteDemoMessage (&net_message);

// finish up
	fclose (cls.demofile);
	cls.demofile = NULL;
	cls.demorecording = false;
	Com_Printf ("Completed demo\n");
}


/*
====================
CL_WriteDemoMessage

Dumps the current net message, prefixed by the length and view angles
====================
*/
void CL_WriteRecordDemoMessage (sizebuf_t *msg, int seq)
{
	int		len;
	int		i;
	float	fl;
	byte	c;

//Com_Printf ("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	if (!cls.demorecording)
		return;

	fl = LittleFloat((float)realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_read;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	len = LittleLong (msg->cursize + 8);
	fwrite (&len, 4, 1, cls.demofile);

	i = LittleLong(seq);
	fwrite (&i, 4, 1, cls.demofile);
	fwrite (&i, 4, 1, cls.demofile);

	fwrite (msg->data, msg->cursize, 1, cls.demofile);

	fflush (cls.demofile);
}


void CL_WriteSetDemoMessage (void)
{
	int		len;
	float	fl;
	byte	c;

//Com_Printf ("write: %ld bytes, %4.4f\n", msg->cursize, realtime);

	if (!cls.demorecording)
		return;

	fl = LittleFloat((float)realtime);
	fwrite (&fl, sizeof(fl), 1, cls.demofile);

	c = dem_set;
	fwrite (&c, sizeof(c), 1, cls.demofile);

	len = LittleLong(cls.netchan.outgoing_sequence);
	fwrite (&len, 4, 1, cls.demofile);
	len = LittleLong(cls.netchan.incoming_sequence);
	fwrite (&len, 4, 1, cls.demofile);

	fflush (cls.demofile);
}


/*
====================
CL_Record

Called by CL_Record_f and CL_EasyRecord_f
====================
*/
static void CL_Record (void)
{
	sizebuf_t	buf;
	char	buf_data[MAX_MSGLEN];
	int n, i, j;
	char *s;
	entity_t *ent;
	entity_state_t *es, blankes;
	player_info_t *player;
	int seq = 1;

	cls.demorecording = true;

/*-------------------------------------------------*/

// serverdata
	// send the info about the new client to all connected clients
	SZ_Init (&buf, buf_data, sizeof(buf_data));

// send the serverdata
	MSG_WriteByte (&buf, svc_serverdata);
	MSG_WriteLong (&buf, PROTOCOL_VERSION);
	MSG_WriteLong (&buf, cl.servercount);
	MSG_WriteString (&buf, cls.gamedirfile);

	if (cl.spectator)
		MSG_WriteByte (&buf, cl.playernum | 128);
	else
		MSG_WriteByte (&buf, cl.playernum);

	// send full levelname
	MSG_WriteString (&buf, cl.levelname);

	// send the movevars
	MSG_WriteFloat(&buf, movevars.gravity);
	MSG_WriteFloat(&buf, movevars.stopspeed);
	MSG_WriteFloat(&buf, cl.maxspeed);
	MSG_WriteFloat(&buf, movevars.spectatormaxspeed);
	MSG_WriteFloat(&buf, movevars.accelerate);
	MSG_WriteFloat(&buf, movevars.airaccelerate);
	MSG_WriteFloat(&buf, movevars.wateraccelerate);
	MSG_WriteFloat(&buf, movevars.friction);
	MSG_WriteFloat(&buf, movevars.waterfriction);
	MSG_WriteFloat(&buf, cl.entgravity);

	// send music
	MSG_WriteByte (&buf, svc_cdtrack);
	MSG_WriteByte (&buf, 0); // none in demos

	// send server info string
	MSG_WriteByte (&buf, svc_stufftext);
	MSG_WriteString (&buf, va("fullserverinfo \"%s\"\n", cl.serverinfo) );

	// flush packet
	CL_WriteRecordDemoMessage (&buf, seq++);
	SZ_Clear (&buf); 

// soundlist
	MSG_WriteByte (&buf, svc_soundlist);
	MSG_WriteByte (&buf, 0);

	n = 0;
	s = cl.sound_name[n+1];
	while (*s) {
		MSG_WriteString (&buf, s);
		if (buf.cursize > MAX_MSGLEN/2) {
			MSG_WriteByte (&buf, 0);
			MSG_WriteByte (&buf, n);
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf); 
			MSG_WriteByte (&buf, svc_soundlist);
			MSG_WriteByte (&buf, n + 1);
		}
		n++;
		s = cl.sound_name[n+1];
	}
	if (buf.cursize) {
		MSG_WriteByte (&buf, 0);
		MSG_WriteByte (&buf, 0);
		CL_WriteRecordDemoMessage (&buf, seq++);
		SZ_Clear (&buf); 
	}

// modellist
	MSG_WriteByte (&buf, svc_modellist);
	MSG_WriteByte (&buf, 0);

	n = 0;
	s = cl.model_name[n+1];
	while (*s) {
		MSG_WriteString (&buf, s);
		if (buf.cursize > MAX_MSGLEN/2) {
			MSG_WriteByte (&buf, 0);
			MSG_WriteByte (&buf, n);
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf); 
			MSG_WriteByte (&buf, svc_modellist);
			MSG_WriteByte (&buf, n + 1);
		}
		n++;
		s = cl.model_name[n+1];
	}
	if (buf.cursize) {
		MSG_WriteByte (&buf, 0);
		MSG_WriteByte (&buf, 0);
		CL_WriteRecordDemoMessage (&buf, seq++);
		SZ_Clear (&buf); 
	}

// spawnstatic

	for (i = 0; i < cl.num_statics; i++) {
		ent = cl_static_entities + i;

		MSG_WriteByte (&buf, svc_spawnstatic);

		for (j = 1; j < MAX_MODELS; j++)
			if (ent->model == cl.model_precache[j])
				break;
		if (j == MAX_MODELS)
			MSG_WriteByte (&buf, 0);
		else
			MSG_WriteByte (&buf, j);

		MSG_WriteByte (&buf, ent->frame);
		MSG_WriteByte (&buf, 0);
		MSG_WriteByte (&buf, ent->skinnum);
		for (j=0 ; j<3 ; j++)
		{
			MSG_WriteCoord (&buf, ent->origin[j]);
			MSG_WriteAngle (&buf, ent->angles[j]);
		}

		if (buf.cursize > MAX_MSGLEN/2) {
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf); 
		}
	}

// spawnstaticsound
	// static sounds are skipped in demos, life is hard

// baselines

	memset(&blankes, 0, sizeof(blankes));
	for (i = 0; i < MAX_EDICTS; i++) {
		es = cl_baselines + i;

		if (memcmp(es, &blankes, sizeof(blankes))) {
			MSG_WriteByte (&buf,svc_spawnbaseline);		
			MSG_WriteShort (&buf, i);

			MSG_WriteByte (&buf, es->modelindex);
			MSG_WriteByte (&buf, es->frame);
			MSG_WriteByte (&buf, es->colormap);
			MSG_WriteByte (&buf, es->skinnum);
			for (j=0 ; j<3 ; j++)
			{
				MSG_WriteCoord(&buf, es->origin[j]);
				MSG_WriteAngle(&buf, es->angles[j]);
			}

			if (buf.cursize > MAX_MSGLEN/2) {
				CL_WriteRecordDemoMessage (&buf, seq++);
				SZ_Clear (&buf); 
			}
		}
	}

	MSG_WriteByte (&buf, svc_stufftext);
	MSG_WriteString (&buf, va("cmd spawn %i 0\n", cl.servercount) );

	if (buf.cursize) {
		CL_WriteRecordDemoMessage (&buf, seq++);
		SZ_Clear (&buf); 
	}

// send current status of all other players

	for (i = 0; i < MAX_CLIENTS; i++) {
		player = cl.players + i;

		MSG_WriteByte (&buf, svc_updatefrags);
		MSG_WriteByte (&buf, i);
		MSG_WriteShort (&buf, player->frags);
		
		MSG_WriteByte (&buf, svc_updateping);
		MSG_WriteByte (&buf, i);
		MSG_WriteShort (&buf, player->ping);
		
		MSG_WriteByte (&buf, svc_updatepl);
		MSG_WriteByte (&buf, i);
		MSG_WriteByte (&buf, player->pl);
		
		MSG_WriteByte (&buf, svc_updateentertime);
		MSG_WriteByte (&buf, i);
		MSG_WriteFloat (&buf, realtime - player->entertime);

		MSG_WriteByte (&buf, svc_updateuserinfo);
		MSG_WriteByte (&buf, i);
		MSG_WriteLong (&buf, player->userid);
		MSG_WriteString (&buf, player->userinfo);

		if (buf.cursize > MAX_MSGLEN/2) {
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf); 
		}
	}
	
// send all current light styles
	for (i=0 ; i<MAX_LIGHTSTYLES ; i++)
	{
		if (!cl_lightstyle[i].length)
			continue;		// don't send empty lightstyle strings
		MSG_WriteByte (&buf, svc_lightstyle);
		MSG_WriteByte (&buf, (char)i);
		MSG_WriteString (&buf, cl_lightstyle[i].map);
	}

	for (i = 0; i < MAX_CL_STATS; i++) {
		if (!cl.stats[i])
			continue;		// no need to send zero values
		if (cl.stats[i] >= 0 && cl.stats[i] <= 255) {
			MSG_WriteByte (&buf, svc_updatestat);
			MSG_WriteByte (&buf, i);
			MSG_WriteByte (&buf, cl.stats[i]);
		} else {
			MSG_WriteByte (&buf, svc_updatestatlong);
			MSG_WriteByte (&buf, i);
			MSG_WriteLong (&buf, cl.stats[i]);
		}
		if (buf.cursize > MAX_MSGLEN/2) {
			CL_WriteRecordDemoMessage (&buf, seq++);
			SZ_Clear (&buf); 
		}
	}

	// get the client to check and download skins
	// when that is completed, a begin command will be issued
	MSG_WriteByte (&buf, svc_stufftext);
	MSG_WriteString (&buf, va("skins\n") );

	CL_WriteRecordDemoMessage (&buf, seq++);

	CL_WriteSetDemoMessage();

	// done
}

/*
====================
CL_Record_f

record <demoname>
====================
*/
void CL_Record_f (void)
{
	int		c;
	char	name[MAX_OSPATH];

	c = Cmd_Argc();
	if (c != 2)
	{
		Com_Printf ("record <demoname>\n");
		return;
	}

	if (cls.state != ca_active && cls.state != ca_disconnected) {
		Com_Printf ("Cannot record while connecting.\n");
		return;
	}

	if (cls.demorecording)
		CL_Stop_f();
  
	Q_snprintfz (name, sizeof(name), "%s/%s", cls.gamedir, Cmd_Argv(1));

//
// open the demo file
//
	COM_ForceExtension (name, ".qwd");

	cls.demofile = fopen (name, "wb");
	if (!cls.demofile)
	{
		Com_Printf ("ERROR: couldn't open.\n");
		return;
	}

	Com_Printf ("recording to %s.\n", name);

	if (cls.state == ca_active)
		CL_Record ();
	else
		cls.demorecording = true;
}


/*
====================
CL_EasyRecord_f

easyrecord [demoname]
====================
*/
void CL_EasyRecord_f (void)
{
	int		c;
	char	name[1024];
	char	name2[MAX_OSPATH*2];
	int		i;
	unsigned char	*p;
	FILE	*f;

	c = Cmd_Argc();
	if (c > 2)
	{
		Com_Printf ("easyrecord <demoname>\n");
		return;
	}

	if (cls.state != ca_active) {
		Com_Printf ("You must be connected to record.\n");
		return;
	}

	if (cls.demorecording)
		CL_Stop_f();

/// FIXME: check buffer sizes!!!

	if (c == 2)
		Q_snprintfz (name, sizeof(name), "%s", Cmd_Argv(1));
	else if (cl.spectator) {
		// FIXME: if tracking a player, use his name
		Q_snprintfz (name, sizeof(name), "spec_%s_%s",
			TP_PlayerName(),
			TP_MapName());
	} else {
		// guess game type and write demo name
		i = TP_CountPlayers();
		if (cl.teamplay && i >= 3)
		{
			// Teamplay
			Q_snprintfz (name, sizeof(name), "%s_%s_vs_%s_%s",
				TP_PlayerName(),
				TP_PlayerTeam(),
				TP_EnemyTeam(),
				TP_MapName());
		} else {
			if (i == 2) {
				// Duel
				Q_snprintfz (name, sizeof(name), "%s_vs_%s_%s",
					TP_PlayerName(),
					TP_EnemyName(),
					TP_MapName());
			}
			else if (i > 2) {
				// FFA
				Q_snprintfz (name, sizeof(name), "%s_ffa_%s",
					TP_PlayerName(), 
					TP_MapName());
			}
			else {
				// one player
				Q_snprintfz (name, sizeof(name), "%s_%s",
					TP_PlayerName(),
					TP_MapName());
			}
		}
	}

// Make sure the filename doesn't contain illegal characters
	for (p=name ; *p ; p++)	{
		char c;
		*p &= 0x7F;		// strip high bit
		c = *p;
		if (c<=' ' || c=='?' || c=='*' || c=='\\' || c=='/' || c==':'
			|| c=='<' || c=='>' || c=='"')
			*p = '_';
	}

	Q_strncpyz (name, va("%s/%s", cls.gamedir, name), MAX_OSPATH);

// find a filename that doesn't exist yet
	strcpy (name2, name);
	COM_ForceExtension (name2, ".qwd");
	f = fopen (name2, "rb");
	if (f) {
		i = 0;
		do {
			fclose (f);
			strcpy (name2, va("%s_%02i", name, i));
			COM_ForceExtension (name2, ".qwd");
			f = fopen (name2, "rb");
			i++;
		} while (f);
	}

//
// open the demo file
//
	cls.demofile = fopen (name2, "wb");
	if (!cls.demofile)
	{
		Com_Printf ("ERROR: couldn't open.\n");
		return;
	}

	Com_Printf ("recording to %s.\n", name2);
	CL_Record ();
}


//==================================================================
// .QWZ demos playback (via Qizmo)
//

#ifdef _WIN32
static void CheckQizmoCompletion ()
{
	DWORD ExitCode;

	if (!hQizmoProcess)
		return;

	if (!GetExitCodeProcess (hQizmoProcess, &ExitCode)) {
		Com_Printf ("WARINING: GetExitCodeProcess failed\n");
		hQizmoProcess = NULL;
		qwz_unpacking = false;
		qwz_playback = false;
		cls.demoplayback = cls.timedemo = false;
		StopQWZPlayback ();
		return;
	}
	
	if (ExitCode == STILL_ACTIVE)
		return;

	hQizmoProcess = NULL;

	if (!qwz_unpacking || !cls.demoplayback) {
		StopQWZPlayback ();
		return;
	}
	
	qwz_unpacking = false;
	
	cls.demofile = fopen (tempqwd_name, "rb");
	if (!cls.demofile) {
		Com_Printf ("Couldn't open %s\n", tempqwd_name);
		qwz_playback = false;
		cls.demoplayback = cls.timedemo = false;
		return;
	}

	// start playback
	cls.demoplayback = true;
	cls.state = ca_demostart;
	Netchan_Setup (NS_CLIENT, &cls.netchan, net_from, 0);
	realtime = 0;
}

static void StopQWZPlayback ()
{
	if (!hQizmoProcess && tempqwd_name[0]) {
		if (remove (tempqwd_name) != 0)
			Com_Printf ("Couldn't delete %s\n", tempqwd_name);
		tempqwd_name[0] = '\0';
	}
	qwz_playback = false;
}


void PlayQWZDemo (void)
{
	extern cvar_t	qizmo_dir;
	STARTUPINFO			si;
	PROCESS_INFORMATION	pi;
	char	*name;
	char	qwz_name[256];
	char	cmdline[512];
	char	*p;

	if (hQizmoProcess) {
		Com_Printf ("Cannot unpack -- Qizmo still running!\n");
		return;
	}
	
	name = Cmd_Argv(1);

	if (!strncmp(name, "../", 3) || !strncmp(name, "..\\", 3))
		Q_strncpyz (qwz_name, va("%s/%s", com_basedir, name+3), sizeof(qwz_name));
	else
		if (name[0] == '/' || name[0] == '\\')
			Q_strncpyz (qwz_name, va("%s/%s", cls.gamedir, name+1), sizeof(qwz_name));
		else
			Q_strncpyz (qwz_name, va("%s/%s", cls.gamedir, name), sizeof(qwz_name));

	// check if the file exists
	cls.demofile = fopen (qwz_name, "rb");
	if (!cls.demofile)
	{
		Com_Printf ("Couldn't open %s\n", name);
		return;
	}
	fclose (cls.demofile);
	
	Q_strncpyz (tempqwd_name, qwz_name, sizeof(tempqwd_name)-4);
#if 0
	// the right way
	strcpy (tempqwd_name + strlen(tempqwd_name) - 4, ".qwd");
#else
	// the way Qizmo does it, sigh
	p = strstr (tempqwd_name, ".qwz");
	if (!p)
		p = strstr (tempqwd_name, ".QWZ");
	if (!p)
		p = tempqwd_name + strlen(tempqwd_name);
	strcpy (p, ".qwd");
#endif

	cls.demofile = fopen (tempqwd_name, "rb");
	if (cls.demofile) {
		// .qwd already exists, so just play it
		cls.demoplayback = true;
		cls.state = ca_demostart;
		Netchan_Setup (NS_CLIENT, &cls.netchan, net_from, 0);
		realtime = 0;
		return;
	}
	
	Com_Printf ("Unpacking %s...\n", COM_SkipPath(name));
	
	// start Qizmo to unpack the demo
	memset (&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.wShowWindow = SW_HIDE;
	si.dwFlags = STARTF_USESHOWWINDOW;
	
	Q_strncpyz (cmdline, va("%s/%s/qizmo.exe -q -u -D \"%s\"", com_basedir,
		qizmo_dir.string, qwz_name), sizeof(cmdline));
	
	if (!CreateProcess (NULL, cmdline, NULL, NULL,
		FALSE, 0/* | HIGH_PRIORITY_CLASS*/,
		NULL, va("%s/%s", com_basedir, qizmo_dir.string), &si, &pi))
	{
		Com_Printf ("Couldn't execute %s/%s/qizmo.exe\n",
			com_basedir, qizmo_dir.string);
		return;
	}
	
	hQizmoProcess = pi.hProcess;
	qwz_unpacking = true;
	qwz_playback = true;

	// demo playback doesn't actually start yet, we just set
	// cls.demoplayback so that CL_StopPlayback() will be called
	// if CL_Disconnect() is issued
	cls.demoplayback = true;
	cls.demofile = NULL;
	cls.state = ca_demostart;
	Netchan_Setup (NS_CLIENT, &cls.netchan, net_from, 0);
	realtime = 0;
}
#endif

/*
====================
CL_PlayDemo_f

play [demoname]
====================
*/
void CL_PlayDemo_f (void)
{
	char	name[256];

	if (Cmd_Argc() != 2)
	{
		Com_Printf ("play <demoname> : plays a demo\n");
		return;
	}

//
// disconnect from server
//
	CL_Disconnect ();
	
//
// open the demo file
//
	Q_strncpyz (name, Cmd_Argv(1), sizeof(name)-4);

#ifdef _WIN32
	if (strlen(name) > 4 && !Q_strcasecmp(name + strlen(name) - 4, ".qwz")) {
		PlayQWZDemo ();
		return;
	}
#endif

	COM_DefaultExtension (name, ".qwd");

	Com_Printf ("Playing demo from %s.\n", COM_SkipPath(name));

	if (!strncmp(name, "../", 3) || !strncmp(name, "..\\", 3))
		cls.demofile = fopen (va("%s/%s", com_basedir, name+3), "rb");
	else
		FS_FOpenFile (name, &cls.demofile);

	if (!cls.demofile)
	{
		Com_Printf ("ERROR: couldn't open.\n");
		cls.demonum = -1;		// stop demo loop
		return;
	}

	cls.demoplayback = true;
	cls.state = ca_demostart;
	Netchan_Setup (NS_CLIENT, &cls.netchan, net_from, 0);
	realtime = 0;
}

/*
====================
CL_FinishTimeDemo

====================
*/
void CL_FinishTimeDemo (void)
{
	int		frames;
	float	time;
	
	cls.timedemo = false;
	
// the first frame didn't count
	frames = (cls.framecount - cls.td_startframe) - 1;
	time = Sys_DoubleTime() - cls.td_starttime;
	if (!time)
		time = 1;
	Com_Printf ("%i frames %5.1f seconds %5.1f fps\n", frames, time, frames/time);
}

/*
====================
CL_TimeDemo_f

timedemo [demoname]
====================
*/
void CL_TimeDemo_f (void)
{
	if (Cmd_Argc() != 2)
	{
		Com_Printf ("timedemo <demoname> : gets demo speeds\n");
		return;
	}

	CL_PlayDemo_f ();
	
	if (cls.state != ca_demostart)
		return;

// cls.td_starttime will be grabbed at the second frame of the demo, so
// all the loading time doesn't get counted
	
	cls.timedemo = true;
	cls.td_starttime = 0;
	cls.td_startframe = cls.framecount;
	cls.td_lastframe = -1;		// get a new message this frame
}
