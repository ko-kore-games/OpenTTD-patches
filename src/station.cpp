/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file station.cpp Implementation of the station base class. */

#include "stdafx.h"
#include "company_func.h"
#include "company_base.h"
#include "roadveh.h"
#include "viewport_func.h"
#include "date_func.h"
#include "command_func.h"
#include "news_func.h"
#include "aircraft.h"
#include "vehiclelist.h"
#include "core/pool_func.hpp"
#include "station_base.h"
#include "roadstop_base.h"
#include "industry.h"
#include "core/random_func.hpp"
#include "linkgraph/linkgraph.h"
#include "linkgraph/linkgraphschedule.h"

#include "table/strings.h"

#include "safeguards.h"

#include "tunnelbridge_map.h"
#include "tunnelbridge.h"

/** The pool of stations. */
StationPool _station_pool("Station");
INSTANTIATE_POOL_METHODS(Station)

BaseStation::~BaseStation()
{
	free(this->name);
	free(this->speclist);

	if (CleaningPool()) return;

	DeleteWindowById(WC_TRAINS_LIST,   VehicleListIdentifier(VL_STATION_LIST, VEH_TRAIN,    this->owner, this->index).Pack());
	DeleteWindowById(WC_ROADVEH_LIST,  VehicleListIdentifier(VL_STATION_LIST, VEH_ROAD,     this->owner, this->index).Pack());
	DeleteWindowById(WC_SHIPS_LIST,    VehicleListIdentifier(VL_STATION_LIST, VEH_SHIP,     this->owner, this->index).Pack());
	DeleteWindowById(WC_AIRCRAFT_LIST, VehicleListIdentifier(VL_STATION_LIST, VEH_AIRCRAFT, this->owner, this->index).Pack());

	this->sign.MarkDirty();
}

Station::Station(TileIndex tile) :
	SpecializedStation<Station, false>(tile),
	bus_station(INVALID_TILE, 0, 0),
	truck_station(INVALID_TILE, 0, 0),
	dock_tile(INVALID_TILE),
	indtype(IT_INVALID),
	time_since_load(255),
	time_since_unload(255),
	last_vehicle_type(VEH_INVALID)
{
	/* this->random_bits is set in Station::AddFacility() */
}

/**
 * Clean up a station by clearing vehicle orders, invalidating windows and
 * removing link stats.
 * Aircraft-Hangar orders need special treatment here, as the hangars are
 * actually part of a station (tiletype is STATION), but the order type
 * is OT_GOTO_DEPOT.
 */
Station::~Station()
{
	if (CleaningPool()) {
		for (CargoID c = 0; c < NUM_CARGO; c++) {
			this->goods[c].cargo.OnCleanPool();
		}
		return;
	}

	while (!this->loading_vehicles.empty()) {
		this->loading_vehicles.front()->LeaveStation();
	}

	Aircraft *a;
	FOR_ALL_AIRCRAFT(a) {
		if (!a->IsNormalAircraft()) continue;
		if (a->targetairport == this->index) a->targetairport = INVALID_STATION;
	}

	for (CargoID c = 0; c < NUM_CARGO; ++c) {
		LinkGraph *lg = LinkGraph::GetIfValid(this->goods[c].link_graph);
		if (lg == NULL) continue;

		for (NodeID node = 0; node < lg->Size(); ++node) {
			Station *st = Station::Get((*lg)[node].Station());
			st->goods[c].flows.erase(this->index);
			if ((*lg)[node][this->goods[c].node].LastUpdate() != INVALID_DATE) {
				st->goods[c].flows.DeleteFlows(this->index);
				RerouteCargo(st, c, this->index, st->index);
			}
		}
		lg->RemoveNode(this->goods[c].node);
		if (lg->Size() == 0) {
			LinkGraphSchedule::instance.Unqueue(lg);
			delete lg;
		}
	}

	Vehicle *v;
	FOR_ALL_VEHICLES(v) {
		/* Forget about this station if this station is removed */
		if (v->last_station_visited == this->index) {
			v->last_station_visited = INVALID_STATION;
		}
		if (v->last_loading_station == this->index) {
			v->last_loading_station = INVALID_STATION;
		}
	}

	/* Clear the persistent storage. */
	delete this->airport.psa;

	if (this->owner == OWNER_NONE) {
		/* Invalidate all in case of oil rigs. */
		InvalidateWindowClassesData(WC_STATION_LIST, 0);
	} else {
		InvalidateWindowData(WC_STATION_LIST, this->owner, 0);
	}

	DeleteWindowById(WC_STATION_VIEW, index);

	/* Now delete all orders that go to the station */
	RemoveOrderFromAllVehicles(OT_GOTO_STATION, this->index);

	/* Remove all news items */
	DeleteStationNews(this->index);

	for (CargoID c = 0; c < NUM_CARGO; c++) {
		this->goods[c].cargo.Truncate();
	}

	CargoPacket::InvalidateAllFrom(this->index);
}


/**
 * Invalidating of the JoinStation window has to be done
 * after removing item from the pool.
 * @param index index of deleted item
 */
void BaseStation::PostDestructor(size_t index)
{
	InvalidateWindowData(WC_SELECT_STATION, 0, 0);
}

/**
 * Get the primary road stop (the first road stop) that the given vehicle can load/unload.
 * @param v the vehicle to get the first road stop for
 * @return the first roadstop that this vehicle can load at
 */
RoadStop *Station::GetPrimaryRoadStop(const RoadVehicle *v) const
{
	RoadStop *rs = this->GetPrimaryRoadStop(v->IsBus() ? ROADSTOP_BUS : ROADSTOP_TRUCK);

	for (; rs != NULL; rs = rs->next) {
		/* The vehicle cannot go to this roadstop (different roadtype) */
		if ((GetRoadTypes(rs->xy) & v->compatible_roadtypes) == ROADTYPES_NONE) continue;
		/* The vehicle is articulated and can therefore not go to a standard road stop. */
		if (IsStandardRoadStopTile(rs->xy) && v->HasArticulatedPart()) continue;

		/* The vehicle can actually go to this road stop. So, return it! */
		break;
	}

	return rs;
}

/**
 * Called when new facility is built on the station. If it is the first facility
 * it initializes also 'xy' and 'random_bits' members
 */
void Station::AddFacility(StationFacility new_facility_bit, TileIndex facil_xy)
{
	if (this->facilities == FACIL_NONE) {
		this->xy = facil_xy;
		this->random_bits = Random();
	}
	this->facilities |= new_facility_bit;
	this->owner = _current_company;
	this->build_date = _date;
}

/**
 * Marks the tiles of the station as dirty.
 *
 * @ingroup dirty
 */
void Station::MarkTilesDirty(bool cargo_change) const
{
	TileIndex tile = this->train_station.tile;
	int w, h;

	if (tile == INVALID_TILE) return;

	/* cargo_change is set if we're refreshing the tiles due to cargo moving
	 * around. */
	if (cargo_change) {
		/* Don't waste time updating if there are no custom station graphics
		 * that might change. Even if there are custom graphics, they might
		 * not change. Unfortunately we have no way of telling. */
		if (this->num_specs == 0) return;
	}

	for (h = 0; h < train_station.h; h++) {
		for (w = 0; w < train_station.w; w++) {
			if (this->TileBelongsToRailStation(tile)) {
				MarkTileDirtyByTile(tile);
			}
			tile += TileDiffXY(1, 0);
		}
		tile += TileDiffXY(-w, 1);
	}
}

/* BEGIN of OpenTTD 1.9.1 stable code: */
/* virtual / uint Station::GetPlatformLength(TileIndex tile) const
{
	assert(this->TileBelongsToRailStation(tile));

	TileIndexDiff delta = (GetRailStationAxis(tile) == AXIS_X ? TileDiffXY(1, 0) : TileDiffXY(0, 1));

	TileIndex t = tile;
	uint len = 0;
	do {
		t -= delta;
		len++;
	} while (IsCompatibleTrainStationTile(t, tile));

	t = tile;
	do {
		t += delta;
		len++;
	} while (IsCompatibleTrainStationTile(t, tile));

	return len - 1;
}
// END of OpenTTD 1.9.1 stable code */

/* BEGIN of OpenTTD 1.9.1 stable code: */
/* virtual / uint Station::GetPlatformLength(TileIndex tile, DiagDirection dir) const
{
	TileIndex start_tile = tile;
	uint length = 0;
	assert(IsRailStationTile(tile));
	assert(dir < DIAGDIR_END);

	do {
		length++;
		tile += TileOffsByDiagDir(dir);
	} while (IsCompatibleTrainStationTile(tile, start_tile));

	return length;
}
// END of OpenTTD 1.9.1 stable code */

/* BEGIN of Allow existing tunnelbridge to increase "platform length" parameter for directly adjacent station tiles */
/* virtual */ uint Station::GetPlatformLength(TileIndex tile) const   // virtual
{
	// Main idea is: to account into the platform length all cases, enabled by advanced settings.

	// For this stage it's enough to call this func. from station tiles only.
//  for Existing objects tunnels and bridges as stations
	// assert(this->TileBelongsToRailStation(tile));
//  for Existing objects tunnels and bridges as stations // 20190724: // 2nd stage: Allow users to convert objects via UI.
	assert(this->TileBelongsToRailStation(tile) || (IsTileType(tile, MP_TUNNELBRIDGE) && (GetStationIndex(tile) > 0))); // Need to compare additionally with this->StationID ?
	// assert(this->TileBelongsToRailStation(tile) || (IsTileType(tile, MP_TUNNELBRIDGE) && (GetStationIndex(tile) != INVALID_STATION))); // Need to compare additionally with this->StationID ?

	TileIndexDiff delta = (GetRailStationAxis(tile) == AXIS_X ? TileDiffXY(1, 0) : TileDiffXY(0, 1));

	// ADVANCED SETTING Allow existing tunnelbridge
	// to increase "platform length" parameter for directly adjacent station tiles.
	// bool A_SETTING_TBIPL_1;
	// A_SETTING_TBIPL_1 = true;
	// ADVANCED SETTING Allow existing rail UNDER bridge
	// to increase "platform length" parameter for directly adjacent station tiles.
	// bool A_SETTING_RUBIPL_1;
	// A_SETTING_RUBIPL_1 = true;
	// ADVANCED SETTING Allow OTHER station
	// to increase "platform length" parameter for directly adjacent station tiles.
	// I.e. other station of the same type but with other StationIndex.
	// For example, to use 2nd station as WayPoint inside the 1st station.  
	// bool A_SETTING_OSIPL_1;
	// A_SETTING_OSIPL_1 = true;

	bool bb1;
	bb1 = false;

	DiagDirection dd1;
	dd1 = DIAGDIR_END;
	TransportType tt1;
	tt1 = INVALID_TRANSPORT;
	TileIndex t12;
	t12 = tile;
	uint dl1;
	dl1 = 0;

	int reverse_delta_1 = 1;
	TileType ctt1 = MP_CLEAR;

	TileIndex t = tile;
	uint len = 0;

	do {
		reverse_delta_1 = -reverse_delta_1;

		t = tile;

// Begin for Existing objects tunnels and bridges as stations

		// If the 1st tile is MP_TUNNELBRIDGE AND has direction corresponding to delta * reverse_delta_1, then go 1 step back relatively to delta * reverse_delta_1.

		if (IsTileType(tile, MP_TUNNELBRIDGE)) {
			// Need to check tunnelbridge direction and compare it with (delta * reverse_delta_1): 
			// We need the direction of delta to be onto the bridge and into the tunnel. 
			// Get the direction pointing to the other end. 
			dd1 = GetTunnelBridgeDirection(t);
			// (delta == TileDiffXY(1, 0)) : -delta <--> DIAGDIR_NE && +delta <--> DIAGDIR_SW
			// (delta == TileDiffXY(0, 1)) : -delta <--> DIAGDIR_NW && +delta <--> DIAGDIR_SE
			bb1 = ((delta == TileDiffXY(1, 0)) && ((reverse_delta_1 < 0) && (dd1 == DIAGDIR_NE) || (reverse_delta_1 > 0) && (dd1 == DIAGDIR_SW))) ||
				  ((delta == TileDiffXY(0, 1)) && ((reverse_delta_1 < 0) && (dd1 == DIAGDIR_NW) || (reverse_delta_1 > 0) && (dd1 == DIAGDIR_SE)));
			// If the 1st tile is MP_TUNNELBRIDGE then go 1 step back relatively to (delta * reverse_delta_1)
			if ((reverse_delta_1 < 0) && bb1) {
				t += delta;
			} else {
				t -= delta;
			}
		}
// End   for Existing objects tunnels and bridges as stations

		do {

			if (reverse_delta_1 < 0) {
				t -= delta;
			} else {
				t += delta;
			}
			// t = t + reverse_delta_1 * delta;
			len++;
			bb1 = false;

			// This is like "if IsCompatibleTrainStationTile2(t, tile)" but with 
			// tunnels, bridges, rails under bridges,
			// even with other stations as waypoints (inside main station). 

			ctt1 = GetTileType(t);
			// ctt1 = (TileType)GB(_m[t].type, 4, 4);
			switch (ctt1) {
				// default: NOT_REACHED();

				case MP_STATION:
					bb1 = IsCompatibleTrainStationTile(t, tile);
					// The next is a copy from IsCompatibleTrainStationTile(t, tile), but
					// without checking of StationIndex. 
					// bb1 = bb1 || (A_SETTING_OSIPL_1 == true) &&
					bb1 = bb1 || IsRailStationTile(t) && IsCompatibleRail(GetRailType(t), GetRailType(tile)) &&
								GetRailStationAxis(t) == GetRailStationAxis(tile) &&
								!IsStationTileBlocked(t);
								// GetStationIndex(t) == GetStationIndex(tile) &&

					break;

				case MP_RAILWAY:
					// If t is tile with railway of compatible railtype.
					// bb1 = (A_SETTING_RUBIPL_1 == true);
					bb1 = true;
// Begin for Existing objects tunnels and bridges as stations
					// If (HasSignals(t)) then terminate this platform (do not continue it).
					bb1 = bb1 && !HasSignals(t);
// End   for Existing objects tunnels and bridges as stations
					bb1 = bb1 && IsCompatibleRail(GetRailType(t), GetRailType(tile));
					// If rails on t are compatible (parallel) to the current station tile.
					// dd1 = GetRailDirection(t);
					dd1 = (DiagDirection)GB(_m[t].m5, 0, 2);
					// BE CAREFUL: other (non-native for (DiagDirection)-type) directions encoding!
					// if ((dd == DIAGDIR_SE) && (delta == TileDiffXY(1, 0)) ||
						// (dd == DIAGDIR_SW) && (delta == TileDiffXY(0, 1)) ||
						// (dd == DIAGDIR_NW)) {
					// if ((dd == 1) && (delta == TileDiffXY(1, 0)) ||
						// (dd == 2) && (delta == TileDiffXY(0, 1)) ||
						// (dd == 3)) {
					bb1 = bb1 && ((dd1 == 1) && (delta == TileDiffXY(1, 0)) ||
								  (dd1 == 2) && (delta == TileDiffXY(0, 1)) ||
								  (dd1 == 3));
					// Add + 1 condition: is bridge over t (if is, then it's orthogonal automaticaly).
					bb1 = bb1 && IsBridgeAbove(t);
					// bb1 = bb1 && (GB(_m[t].type, 2, 2) != 0);

				// It doesn't exist yet, but it have to be:
				// bb1 = bb1 && !IsRailTileBlocked(t);

					break;

				case MP_TUNNELBRIDGE:
					// If t is tile with entrance to tunnel or bridge.
					// If directions of AXIS_X or AXIS_Y correspond for this tunnelbridge and this station.
					dd1 = GetTunnelBridgeDirection(t);
					// dd1 = (DiagDirection)GB(_m[t].m5, 0, 2);
					tt1 = GetTunnelBridgeTransportType(t);
					// tt1 = (TransportType)GB(_m[t].m5, 2, 2);
					// if ((((dd == DIAGDIR_NE) || (dd == DIAGDIR_SW)) && (delta == TileDiffXY(1, 0)) ||
					// ((dd == DIAGDIR_SE) || (dd == DIAGDIR_NW)) && (delta == TileDiffXY(0, 1))) &&
					// (tt == TRANSPORT_RAIL)) {

					// Without if-s order in sequence of boolean checks is not important (all checks have to be done). 
					// But shorter notation is next: 
// Begin for Existing objects tunnels and bridges as stations
					// bb1 = (((dd1 == DIAGDIR_NE) || (dd1 == DIAGDIR_SW)) && (delta == TileDiffXY(1, 0)) ||
					// 	      ((dd1 == DIAGDIR_SE) || (dd1 == DIAGDIR_NW)) && (delta == TileDiffXY(0, 1)));

					// Need to check tunnelbridge direction and compare it with (sign of) delta: 
					// (delta == TileDiffXY(1, 0)) : -delta <--> DIAGDIR_NE && +delta <--> DIAGDIR_SW
					// (delta == TileDiffXY(0, 1)) : -delta <--> DIAGDIR_NW && +delta <--> DIAGDIR_SE
					bb1 = ((delta == TileDiffXY(1, 0)) && ((reverse_delta_1 < 0) && (dd1 == DIAGDIR_NE) || (reverse_delta_1 > 0) && (dd1 == DIAGDIR_SW))) ||
						  ((delta == TileDiffXY(0, 1)) && ((reverse_delta_1 < 0) && (dd1 == DIAGDIR_NW) || (reverse_delta_1 > 0) && (dd1 == DIAGDIR_SE)));
// End   for Existing objects tunnels and bridges as stations

					bb1 = bb1 && (tt1 == TRANSPORT_RAIL);
					// bb1 = bb1 && (A_SETTING_TBIPL_1 == true);

				// It doesn't exist yet, but it have to be:
				// bb1 = bb1 && !IsTunnelBridgeTileBlocked(t);

					if (bb1) {
						// GetOtherTunnelBridgeEnd(t) contains a cycle (loop), then faster way is to call it only once (and store result).
						t12 = GetOtherTunnelBridgeEnd(t);
						// length of bridge/tunnel middle + 2 (entrances included)
						// GetTunnelBridgeLength(TileIndex begin, TileIndex end);
						dl1 = GetTunnelBridgeLength(t, t12) + 2;
						// dl1 = abs(TileX(t12) - TileX(t) + TileY(t12) - TileY(t)) - 1 + 2;
						// len = len + dl1;
						len += (dl1 - 1); // -1 because +1 tile will be added to this variable on the next loop of the do..while cycle.
						t = t12;
					}
					break;
			}

		// } while (IsCompatibleTrainStationTile(t, tile));
		} while (bb1);
	} while (reverse_delta_1 < 1);
	
	return len - 1;
}
// END of   Allow existing tunnelbridge to increase "platform length" parameter for directly adjacent station tiles */

/* BEGIN of OpenTTD 1.9.1 stable code: */
/* virtual / uint Station::GetPlatformLength(TileIndex tile, DiagDirection dir) const
{
	TileIndex start_tile = tile;
	uint length = 0;
	assert(IsRailStationTile(tile));
	assert(dir < DIAGDIR_END);

	do {
		length++;
		tile += TileOffsByDiagDir(dir);
	} while (IsCompatibleTrainStationTile(tile, start_tile));

	return length;
}
// END of OpenTTD 1.9.1 stable code */

/* BEGIN of Allow existing tunnelbridge to increase "platform length" parameter for directly adjacent station tiles */
/* virtual */ uint Station::GetPlatformLength(TileIndex tile, DiagDirection dir) const
{
	TileIndex start_tile = tile;
	uint length = 0;
//  for Existing objects tunnels and bridges as stations
	// assert(IsRailStationTile(tile));
//  for Existing objects tunnels and bridges as stations // 20190724: // 2nd stage: Allow users to convert objects via UI.
	assert(IsRailStationTile(tile) || (IsTileType(tile, MP_TUNNELBRIDGE) && (GetStationIndex(tile) > 0))); // Need to compare additionally with this->StationID ?
	// assert(IsRailStationTile(tile) || (IsTileType(tile, MP_TUNNELBRIDGE) && (GetStationIndex(tile) != INVALID_STATION))); // Need to compare additionally with this->StationID ?
	assert(dir < DIAGDIR_END);

	bool bb2;
	bb2 = false;
	TileType ctt2;
	ctt2 = MP_CLEAR;
	TileIndexDiff tid2;
	tid2 = TileOffsByDiagDir(dir);

	uint length2;
	length2 = 0;
	TileIndex tile2;
	tile2 = tile;
/**/
	DiagDirection dd2;
	dd2 = DIAGDIR_END;
	TransportType tt2;
	tt2 = INVALID_TRANSPORT;
	TileIndex t22;
	t22 = tile;
	uint dl2;
	dl2 = 0;
/**/

// Begin for Existing objects tunnels and bridges as stations

	// If the 1st tile is MP_TUNNELBRIDGE AND has direction corresponding to dir (tid2), then go 1 step back relatively to dir (tid2).

	if (IsTileType(tile, MP_TUNNELBRIDGE)) {
		// Need to check tunnelbridge direction and compare it with dir (tid2):
		// We need the direction of dir (delta, tid2) to be onto the bridge and into the tunnel. 
		// Get the direction pointing to the other end. 
		dd2 = GetTunnelBridgeDirection(tile2);
		// (delta == TileDiffXY(1, 0)) : -delta <--> DIAGDIR_NE && +delta <--> DIAGDIR_SW
		// (delta == TileDiffXY(0, 1)) : -delta <--> DIAGDIR_NW && +delta <--> DIAGDIR_SE
		bb2 = ((tid2 == -TileDiffXY(1, 0)) && (dd2 == DIAGDIR_NE)) || ((tid2 == TileDiffXY(1, 0)) && (dd2 == DIAGDIR_SW)) ||
			  ((tid2 == -TileDiffXY(0, 1)) && (dd2 == DIAGDIR_NW)) || ((tid2 == TileDiffXY(0, 1)) && (dd2 == DIAGDIR_SE));
		if (bb2) {
			tile2 -= tid2;
		}
	}
// End   for Existing objects tunnels and bridges as stations

	do {
//		length2++;
		length++;
		tile2 += tid2;
		bb2 = false;

		// This is like "if IsCompatibleTrainStationTile2(t, tile)" but with 
		// tunnels, bridges, rails under bridges,
		// even with other stations as waypoints (inside main station). 

		// ctt2 = GetTileType(tile);
		ctt2 = GetTileType(tile2);
		// ctt2 = (TileType)GB(_m[tile2].type, 4, 4);
		switch (ctt2) {
			// default: NOT_REACHED();

			case MP_STATION:
				// bb2 = IsCompatibleTrainStationTile(tile, start_tile);
				bb2 = IsCompatibleTrainStationTile(tile2, start_tile);
				if (bb2) {
					// if (IsCompatibleTrainStationTile(tile2, start_tile)) {
						// Everytime a complette compatible station tile is the signal 
						// to accept previously passed tiles into account of platform length (AHEAD! - it's important), 
						// because they are between the start_tile and this (complette compatible) station tile. 
						// length = length + length2;
//					length += length2;

					// Don't know if this is needed. If don't, then we can don't use the variable tile2 at all. 
//					tile2 = tile;
					tile = tile2;
//					tile += tid2 * length2;
					length2 = 0;
				} else {
					// The next is a copy from IsCompatibleTrainStationTile(tile, start_tile), but
					// without checking of StationIndex. 
					// bb2 = (A_SETTING_OSIPL_2 == true) &&
					bb2 = true;
					bb2 = bb2 && IsRailStationTile(tile2) && IsCompatibleRail(GetRailType(tile2), GetRailType(start_tile)) &&
						GetRailStationAxis(tile2) == GetRailStationAxis(start_tile) &&
						!IsStationTileBlocked(tile2);
						// GetStationIndex(tile2) == GetStationIndex(start_tile) &&
					if (bb2) { length2++; }
				}
				// Maybe, we could enable diagonal platforms too?
// /* /
				break;
/* / */
			case MP_RAILWAY:
				// If t (tile2) is tile with railway of compatible railtype.
				// bb2 = (A_SETTING_RUBIPL_2 == true);
				bb2 = true;
// Begin for Existing objects tunnels and bridges as stations
				// If (HasSignals(t)) then terminate this platform (do not continue it).
				bb2 = bb2 && !HasSignals(tile2);
// End   for Existing objects tunnels and bridges as stations
				bb2 = bb2 && IsCompatibleRail(GetRailType(tile2), GetRailType(start_tile));
				// If rails on t (tile2) are compatible (parallel) to the current station tile.
				// dd2 = GetRailsDirection(tile2);
				dd2 = (DiagDirection)GB(_m[tile2].m5, 0, 2);
				// BE CAREFUL: other (non-native for (DiagDirection)-type) directions encoding!
				// if ((dd == DIAGDIR_SE) && (delta == TileDiffXY(1, 0)) ||
					// (dd == DIAGDIR_SW) && (delta == TileDiffXY(0, 1)) ||
					// (dd == DIAGDIR_NW)) {
				// if ((dd == 1) && (delta == TileDiffXY(1, 0)) ||
					// (dd == 2) && (delta == TileDiffXY(0, 1)) ||
					// (dd == 3)) {
				// BE CAREFUL: other formula (including 2 directions relative to tid):
				bb2 = bb2 && ((dd2 == 1) && ((tid2 == TileDiffXY(1, 0)) || (tid2 == -TileDiffXY(1, 0))) ||
							  (dd2 == 2) && ((tid2 == TileDiffXY(0, 1)) || (tid2 == -TileDiffXY(0, 1))) ||
							  (dd2 == 3));
			// Maybe, we could enable diagonal rails too?

				// Add + 1 condition: is bridge over t (if is, then it's orthogonal automaticaly).
				bb2 = bb2 && IsBridgeAbove(tile2);
				// bb2 = bb2 && (GB(_m[tile2].type, 2, 2) != 0);

			// Here we can add +1 condition on type of bridge to enable only bridges with graphics of stations on them. 
				
				// It doesn't exist yet, but it have to be:
				// bb2 = bb2 && !IsRailTileBlocked(tile2);
				if (bb2) { length2++; }
				break;
//* */
/* / */
			case MP_TUNNELBRIDGE:
				// If t (tile2) is tile with entrance to tunnel or bridge.
				// If directions of AXIS_X or AXIS_Y correspond for this tunnelbridge and this station.
				dd2 = GetTunnelBridgeDirection(tile2);
				// dd2 = (DiagDirection)GB(_m[tile2].m5, 0, 2);
				tt2 = GetTunnelBridgeTransportType(tile2);
				// tt2 = (TransportType)GB(_m[tile2].m5, 2, 2);
				// if ((((dd == DIAGDIR_NE) || (dd == DIAGDIR_SW)) && (delta == TileDiffXY(1, 0)) ||
				// ((dd == DIAGDIR_SE) || (dd == DIAGDIR_NW)) && (delta == TileDiffXY(0, 1))) &&
				// (tt == TRANSPORT_RAIL)) {

				// Without if-s order in sequence of boolean checks is not important (all checks have to be done).
				// But shorter notation is next:

				// start_tile can not be a platform tile UNDER any TUNNEL or ABOVE any BRIDGE, then
				// any tile2 of type MP_TUNNELBRIDGE can be only an entrance to the tunnel or bridge.
				// But formula is other because of 2 possible directions of tid (parallel to tid).
// Begin for Existing objects tunnels and bridges as stations
				// bb2 = (((dd2 == DIAGDIR_NE) || (dd2 == DIAGDIR_SW)) && ((tid2 == TileDiffXY(1, 0)) || (tid2 == -TileDiffXY(1, 0))) ||
				//		  ((dd2 == DIAGDIR_SE) || (dd2 == DIAGDIR_NW)) && ((tid2 == TileDiffXY(0, 1)) || (tid2 == -TileDiffXY(0, 1))));

				// (delta == TileDiffXY(1, 0)) : -delta <--> DIAGDIR_NE && +delta <--> DIAGDIR_SW
				// (delta == TileDiffXY(0, 1)) : -delta <--> DIAGDIR_NW && +delta <--> DIAGDIR_SE
				bb2 = ((tid2 == -TileDiffXY(1, 0)) && (dd2 == DIAGDIR_NE)) || ((tid2 == TileDiffXY(1, 0)) && (dd2 == DIAGDIR_SW)) ||
					  ((tid2 == -TileDiffXY(0, 1)) && (dd2 == DIAGDIR_NW)) || ((tid2 == TileDiffXY(0, 1)) && (dd2 == DIAGDIR_SE));
// End   for Existing objects tunnels and bridges as stations

				bb2 = bb2 && (tt2 == TRANSPORT_RAIL);
				// bb2 = bb2 && (A_SETTING_TBIPL_2 == true);

				// It doesn't exist yet, but it have to be:
				// bb2 = bb2 && !IsTunnelBridgeTileBlocked(tile2);

				if (bb2) {
					// GetOtherTunnelBridgeEnd(t) contains a cycle (loop), then faster way is to call it only once (and store result).
					t22 = GetOtherTunnelBridgeEnd(tile2);
					// length of bridge/tunnel middle + 2 (entrances included)
					// GetTunnelBridgeLength(TileIndex begin, TileIndex end);
					dl2 = GetTunnelBridgeLength(tile2, t22) + 2;
					// dl2 = abs(TileX(t22) - TileX(tile2) + TileY(t22) - TileY(tile2)) - 1 + 2;
					// length2 = length2 + dl2;
					length += (dl2 - 1); // -1 because +1 tile will be added to this variable on the next loop of the do..while cycle. 
					length2 += dl2;
// Begin for Existing objects tunnels and bridges as stations
					// We need GetStationIndex(t22) > 0 actually at the FAR END of tunnelbridge: 
					// if ((GetStationIndex(tile2) > 0) || (GetStationIndex(t22) > 0)) {
					// if (GetStationIndex(t22) == GetStationIndex(start_tile)) {
					if (true) {
					// Assume that we somehow forget to set the StationIndex value to some tiles with tunnelbridge entrances (dir.adj.to st.tiles),
					// then we can set it here. :$ 
					// if (length > length2) {
//  for Existing objects tunnels and bridges as stations // 20190724: // 2nd stage: Allow users to convert objects via UI.
						if (false && !(_m[tile2].m2 == _m[start_tile].m2)) { // No need for this after // 20190724: // 2nd stage: Allow users to convert objects via UI.
						// if (!(_m[tile2].m2 == _m[start_tile].m2)) {
							_m[tile2].m2 = _m[start_tile].m2; // GetStationIndex(start_tile);
							_m[t22].m2 =   _m[start_tile].m2; // GetStationIndex(start_tile);
							// _m[tile2].m4 = _m[start_tile].m4; // GetCustomStationSpecIndex(start_tile);
							// _m[t22].m4 =   _m[start_tile].m4; // GetCustomStationSpecIndex(start_tile);
						}
						length2 = 0;
						tile = t22;
					} else {
//  for Existing objects tunnels and bridges as stations // 20190724: // 2nd stage: Allow users to convert objects via UI.
						if (!(_m[tile2].m2 == 0)) {
						// if (!(_m[tile2].m2 == INVALID_STATION)) {
							_m[tile2].m2 = 0;
							_m[t22].m2 = 0;
							// _m[tile2].m2 = INVALID_STATION;
							// _m[t22].m2 = INVALID_STATION; 
							// _m[tile2].m4 = 0;
							// _m[t22].m4 = 0;
						}
					}
// End   for Existing objects tunnels and bridges as stations
					tile2 = t22;
				}
				break;
//* */
		}

	// } while (IsCompatibleTrainStationTile(tile, start_tile));
	} while (bb2);

	return (length - length2);
}

/* BEGIN of Existing objects tunnels and bridges as stations --> look above /
uint Station::GetPlatformLength(TileIndex tile, int32 z_position) const   // virtual
{
	//    !!!!!!!    z_position needed as argument to check tunnel or bridge stations !!!!!!!

	// if (this->TileBelongsToRailStation(tile))
	// else if (IsTileType(tile, MP_TUNNELBRIDGE))
	// else if ( !((this->TileBelongsToRailStation(tile)) || (IsTileType(tile, MP_TUNNELBRIDGE))) )  // (train->track == TRACK_BIT_WORMHOLE )

	// assert(this->TileBelongsToRailStation(tile) || (IsTileType(tile, MP_TUNNELBRIDGE) && (StationID)_m[tile].m2 > 0));

}
// END of   Existing objects tunnels and bridges as stations */

/**
 * Determines the catchment radius of the station
 * @return The radius
 */
uint Station::GetCatchmentRadius() const
{
	uint ret = CA_NONE;

	if (_settings_game.station.modified_catchment) {
		if (this->bus_stops          != NULL)         ret = max<uint>(ret, CA_BUS);
		if (this->truck_stops        != NULL)         ret = max<uint>(ret, CA_TRUCK);
		if (this->train_station.tile != INVALID_TILE) ret = max<uint>(ret, CA_TRAIN);
		if (this->dock_tile          != INVALID_TILE) ret = max<uint>(ret, CA_DOCK);
		if (this->airport.tile       != INVALID_TILE) ret = max<uint>(ret, this->airport.GetSpec()->catchment);
	} else {
		if (this->bus_stops != NULL || this->truck_stops != NULL || this->train_station.tile != INVALID_TILE || this->dock_tile != INVALID_TILE || this->airport.tile != INVALID_TILE) {
			ret = CA_UNMODIFIED;
		}
	}

	return ret;
}

/**
 * Determines catchment rectangle of this station
 * @return clamped catchment rectangle
 */
Rect Station::GetCatchmentRect() const
{
	assert(!this->rect.IsEmpty());

	/* Compute acceptance rectangle */
	int catchment_radius = this->GetCatchmentRadius();

	Rect ret = {
		max<int>(this->rect.left   - catchment_radius, 0),
		max<int>(this->rect.top    - catchment_radius, 0),
		min<int>(this->rect.right  + catchment_radius, MapMaxX()),
		min<int>(this->rect.bottom + catchment_radius, MapMaxY())
	};

	return ret;
}

/** Rect and pointer to IndustryVector */
struct RectAndIndustryVector {
	Rect rect;                       ///< The rectangle to search the industries in.
	IndustryVector *industries_near; ///< The nearby industries.
};

/**
 * Callback function for Station::RecomputeIndustriesNear()
 * Tests whether tile is an industry and possibly adds
 * the industry to station's industries_near list.
 * @param ind_tile tile to check
 * @param user_data pointer to RectAndIndustryVector
 * @return always false, we want to search all tiles
 */
static bool FindIndustryToDeliver(TileIndex ind_tile, void *user_data)
{
	/* Only process industry tiles */
	if (!IsTileType(ind_tile, MP_INDUSTRY)) return false;

	RectAndIndustryVector *riv = (RectAndIndustryVector *)user_data;
	Industry *ind = Industry::GetByTile(ind_tile);

	/* Don't check further if this industry is already in the list */
	if (riv->industries_near->Contains(ind)) return false;

	/* Only process tiles in the station acceptance rectangle */
	int x = TileX(ind_tile);
	int y = TileY(ind_tile);
	if (x < riv->rect.left || x > riv->rect.right || y < riv->rect.top || y > riv->rect.bottom) return false;

	/* Include only industries that can accept cargo */
	uint cargo_index;
	for (cargo_index = 0; cargo_index < lengthof(ind->accepts_cargo); cargo_index++) {
		if (ind->accepts_cargo[cargo_index] != CT_INVALID) break;
	}
	if (cargo_index >= lengthof(ind->accepts_cargo)) return false;

	*riv->industries_near->Append() = ind;

	return false;
}

/**
 * Recomputes Station::industries_near, list of industries possibly
 * accepting cargo in station's catchment radius
 */
void Station::RecomputeIndustriesNear()
{
	this->industries_near.Clear();
	if (this->rect.IsEmpty()) return;

	RectAndIndustryVector riv = {
		this->GetCatchmentRect(),
		&this->industries_near
	};

	/* Compute maximum extent of acceptance rectangle wrt. station sign */
	TileIndex start_tile = this->xy;
	uint max_radius = max(
		max(DistanceManhattan(start_tile, TileXY(riv.rect.left,  riv.rect.top)), DistanceManhattan(start_tile, TileXY(riv.rect.left,  riv.rect.bottom))),
		max(DistanceManhattan(start_tile, TileXY(riv.rect.right, riv.rect.top)), DistanceManhattan(start_tile, TileXY(riv.rect.right, riv.rect.bottom)))
	);

	CircularTileSearch(&start_tile, 2 * max_radius + 1, &FindIndustryToDeliver, &riv);
}

/**
 * Recomputes Station::industries_near for all stations
 */
/* static */ void Station::RecomputeIndustriesNearForAll()
{
	Station *st;
	FOR_ALL_STATIONS(st) st->RecomputeIndustriesNear();
}

/************************************************************************/
/*                     StationRect implementation                       */
/************************************************************************/

StationRect::StationRect()
{
	this->MakeEmpty();
}

void StationRect::MakeEmpty()
{
	this->left = this->top = this->right = this->bottom = 0;
}

/**
 * Determines whether a given point (x, y) is within a certain distance of
 * the station rectangle.
 * @note x and y are in Tile coordinates
 * @param x X coordinate
 * @param y Y coordinate
 * @param distance The maximum distance a point may have (L1 norm)
 * @return true if the point is within distance tiles of the station rectangle
 */
bool StationRect::PtInExtendedRect(int x, int y, int distance) const
{
	return this->left - distance <= x && x <= this->right + distance &&
			this->top - distance <= y && y <= this->bottom + distance;
}

bool StationRect::IsEmpty() const
{
	return this->left == 0 || this->left > this->right || this->top > this->bottom;
}

CommandCost StationRect::BeforeAddTile(TileIndex tile, StationRectMode mode)
{
	int x = TileX(tile);
	int y = TileY(tile);
	if (this->IsEmpty()) {
		/* we are adding the first station tile */
		if (mode != ADD_TEST) {
			this->left = this->right = x;
			this->top = this->bottom = y;
		}
	} else if (!this->PtInExtendedRect(x, y)) {
		/* current rect is not empty and new point is outside this rect
		 * make new spread-out rectangle */
		Rect new_rect = {min(x, this->left), min(y, this->top), max(x, this->right), max(y, this->bottom)};

		/* check new rect dimensions against preset max */
		int w = new_rect.right - new_rect.left + 1;
		int h = new_rect.bottom - new_rect.top + 1;
		if (mode != ADD_FORCE && (w > _settings_game.station.station_spread || h > _settings_game.station.station_spread)) {
			assert(mode != ADD_TRY);
			return_cmd_error(STR_ERROR_STATION_TOO_SPREAD_OUT);
		}

		/* spread-out ok, return true */
		if (mode != ADD_TEST) {
			/* we should update the station rect */
			*this = new_rect;
		}
	} else {
		; // new point is inside the rect, we don't need to do anything
	}
	return CommandCost();
}

CommandCost StationRect::BeforeAddRect(TileIndex tile, int w, int h, StationRectMode mode)
{
	if (mode == ADD_FORCE || (w <= _settings_game.station.station_spread && h <= _settings_game.station.station_spread)) {
		/* Important when the old rect is completely inside the new rect, resp. the old one was empty. */
		CommandCost ret = this->BeforeAddTile(tile, mode);
		if (ret.Succeeded()) ret = this->BeforeAddTile(TILE_ADDXY(tile, w - 1, h - 1), mode);
		return ret;
	}
	return CommandCost();
}

/**
 * Check whether station tiles of the given station id exist in the given rectangle
 * @param st_id    Station ID to look for in the rectangle
 * @param left_a   Minimal tile X edge of the rectangle
 * @param top_a    Minimal tile Y edge of the rectangle
 * @param right_a  Maximal tile X edge of the rectangle (inclusive)
 * @param bottom_a Maximal tile Y edge of the rectangle (inclusive)
 * @return \c true if a station tile with the given \a st_id exists in the rectangle, \c false otherwise
 */
/* static */ bool StationRect::ScanForStationTiles(StationID st_id, int left_a, int top_a, int right_a, int bottom_a)
{
	TileArea ta(TileXY(left_a, top_a), TileXY(right_a, bottom_a));
	TILE_AREA_LOOP(tile, ta) {
//  for Existing objects tunnels and bridges as stations
		// if (IsTileType(tile, MP_STATION) && GetStationIndex(tile) == st_id) return true;
		if ((IsTileType(tile, MP_STATION) || IsTileType(tile, MP_TUNNELBRIDGE)) && GetStationIndex(tile) == st_id) return true;
	}

	return false;
}

bool StationRect::AfterRemoveTile(BaseStation *st, TileIndex tile)
{
	int x = TileX(tile);
	int y = TileY(tile);

	/* look if removed tile was on the bounding rect edge
	 * and try to reduce the rect by this edge
	 * do it until we have empty rect or nothing to do */
	for (;;) {
		/* check if removed tile is on rect edge */
		bool left_edge = (x == this->left);
		bool right_edge = (x == this->right);
		bool top_edge = (y == this->top);
		bool bottom_edge = (y == this->bottom);

		/* can we reduce the rect in either direction? */
		bool reduce_x = ((left_edge || right_edge) && !ScanForStationTiles(st->index, x, this->top, x, this->bottom));
		bool reduce_y = ((top_edge || bottom_edge) && !ScanForStationTiles(st->index, this->left, y, this->right, y));
		if (!(reduce_x || reduce_y)) break; // nothing to do (can't reduce)

		if (reduce_x) {
			/* reduce horizontally */
			if (left_edge) {
				/* move left edge right */
				this->left = x = x + 1;
			} else {
				/* move right edge left */
				this->right = x = x - 1;
			}
		}
		if (reduce_y) {
			/* reduce vertically */
			if (top_edge) {
				/* move top edge down */
				this->top = y = y + 1;
			} else {
				/* move bottom edge up */
				this->bottom = y = y - 1;
			}
		}

		if (left > right || top > bottom) {
			/* can't continue, if the remaining rectangle is empty */
			this->MakeEmpty();
			return true; // empty remaining rect
		}
	}
	return false; // non-empty remaining rect
}

bool StationRect::AfterRemoveRect(BaseStation *st, TileArea ta)
{
	assert(this->PtInExtendedRect(TileX(ta.tile), TileY(ta.tile)));
	assert(this->PtInExtendedRect(TileX(ta.tile) + ta.w - 1, TileY(ta.tile) + ta.h - 1));

	bool empty = this->AfterRemoveTile(st, ta.tile);
	if (ta.w != 1 || ta.h != 1) empty = empty || this->AfterRemoveTile(st, TILE_ADDXY(ta.tile, ta.w - 1, ta.h - 1));
	return empty;
}

StationRect& StationRect::operator = (const Rect &src)
{
	this->left = src.left;
	this->top = src.top;
	this->right = src.right;
	this->bottom = src.bottom;
	return *this;
}

/**
 * Calculates the maintenance cost of all airports of a company.
 * @param owner Company.
 * @return Total cost.
 */
Money AirportMaintenanceCost(Owner owner)
{
	Money total_cost = 0;

	const Station *st;
	FOR_ALL_STATIONS(st) {
		if (st->owner == owner && (st->facilities & FACIL_AIRPORT)) {
			total_cost += _price[PR_INFRASTRUCTURE_AIRPORT] * st->airport.GetSpec()->maintenance_cost;
		}
	}
	/* 3 bits fraction for the maintenance cost factor. */
	return total_cost >> 3;
}
