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
#include "quakedef.h"
#include "pmove.h"

extern	vec3_t player_mins;
extern	vec3_t player_maxs;

/*
==================
PM_PointContents

==================
*/
int PM_PointContents (vec3_t p)
{
	float		d;
	dclipnode_t	*node;
	mplane_t	*plane;
	hull_t		*hull;
	int			num;

	hull = &pmove.physents[0].model->hulls[0];

	num = hull->firstclipnode;

	while (num >= 0)
	{
		if (num < hull->firstclipnode || num > hull->lastclipnode)
			Sys_Error ("PM_PointContents: bad node number");
	
		node = hull->clipnodes + num;
		plane = hull->planes + node->planenum;
		
		if (plane->type < 3)
			d = p[plane->type] - plane->dist;
		else
			d = DotProduct (plane->normal, p) - plane->dist;
		if (d < 0)
			num = node->children[1];
		else
			num = node->children[0];
	}
	
	return num;
}

/*
================
PM_TestPlayerPosition

Returns false if the given player position is not valid (in solid)
================
*/
qboolean PM_TestPlayerPosition (vec3_t pos)
{
	int			i;
	physent_t	*pe;
	vec3_t		mins, maxs, test;
	hull_t		*hull;

	for (i=0 ; i< pmove.numphysent ; i++)
	{
		pe = &pmove.physents[i];
	// get the clipping hull
		if (pe->model)
			hull = &pmove.physents[i].model->hulls[1];
		else
		{
			VectorSubtract (pe->mins, player_maxs, mins);
			VectorSubtract (pe->maxs, player_mins, maxs);
			hull = CM_HullForBox (mins, maxs);
		}

		VectorSubtract (pos, pe->origin, test);

		if (CM_HullPointContents (hull, hull->firstclipnode, test) == CONTENTS_SOLID)
			return false;
	}

	return true;
}

/*
================
PM_PlayerTrace
================
*/
trace_t PM_PlayerTrace (vec3_t start, vec3_t end)
{
	trace_t		trace, total;
	vec3_t		offset;
	vec3_t		start_l, end_l;
	hull_t		*hull;
	int			i;
	physent_t	*pe;
	vec3_t		mins, maxs;

// fill in a default trace
	memset (&total, 0, sizeof(trace_t));
	total.fraction = 1;
	total.entnum = -1;
	VectorCopy (end, total.endpos);

	for (i=0 ; i< pmove.numphysent ; i++)
	{
		pe = &pmove.physents[i];
	// get the clipping hull
		if (pe->model)
			hull = &pmove.physents[i].model->hulls[1];
		else
		{
			VectorSubtract (pe->mins, player_maxs, mins);
			VectorSubtract (pe->maxs, player_mins, maxs);
			hull = CM_HullForBox (mins, maxs);
		}

	// PM_HullForEntity (ent, mins, maxs, offset);
	VectorCopy (pe->origin, offset);

		VectorSubtract (start, offset, start_l);
		VectorSubtract (end, offset, end_l);

		// trace a line through the apropriate clipping hull
		trace = CM_HullTrace (hull, start_l, end_l);

		// fix trace up by the offset
		VectorAdd (trace.endpos, offset, trace.endpos);

		if (trace.allsolid)
			trace.startsolid = true;
		if (trace.startsolid)
			trace.fraction = 0;

		// did we clip the move?
		if (trace.fraction < total.fraction)
		{
			total = trace;
			total.entnum = i;
		}

	}

	return total;
}


/*
================
PM_TraceLine
================
*/
trace_t PM_TraceLine (vec3_t start, vec3_t end)
{
	trace_t		trace, total;
	vec3_t		offset;
	vec3_t		start_l, end_l;
	hull_t		*hull;
	int			i;
	physent_t	*pe;

// fill in a default trace
	memset (&total, 0, sizeof(trace_t));
	total.fraction = 1;
	total.entnum = -1;
	VectorCopy (end, total.endpos);

	for (i=0 ; i< pmove.numphysent ; i++)
	{
		pe = &pmove.physents[i];
	// get the clipping hull
		if (pe->model)
			hull = &pmove.physents[i].model->hulls[0];
		else
			hull = CM_HullForBox (pe->mins, pe->maxs);

	// PM_HullForEntity (ent, mins, maxs, offset);
	VectorCopy (pe->origin, offset);

		VectorSubtract (start, offset, start_l);
		VectorSubtract (end, offset, end_l);

		// trace a line through the apropriate clipping hull
		trace = CM_HullTrace (hull, start_l, end_l);

		// fix trace up by the offset
		VectorAdd (trace.endpos, offset, trace.endpos);

		if (trace.allsolid)
			trace.startsolid = true;
		if (trace.startsolid)
			trace.fraction = 0;

		// did we clip the move?
		if (trace.fraction < total.fraction)
		{
			total = trace;
			total.entnum = i;
		}

	}

	return total;
}


