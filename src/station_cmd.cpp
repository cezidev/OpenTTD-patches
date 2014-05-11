/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file station_cmd.cpp Handling of station tiles. */

#include "stdafx.h"
#include "aircraft.h"
#include "cmd_helper.h"
#include "viewport_func.h"
#include "command_func.h"
#include "town.h"
#include "news_func.h"
#include "train.h"
#include "ship.h"
#include "roadveh.h"
#include "industry.h"
#include "newgrf_cargo.h"
#include "newgrf_debug.h"
#include "newgrf_station.h"
#include "newgrf_canal.h" /* For the buoy */
#include "pathfinder/yapf/yapf.h"
#include "road_internal.h" /* For drawing catenary/checking road removal */
#include "autoslope.h"
#include "water.h"
#include "strings_func.h"
#include "clear_func.h"
#include "date_func.h"
#include "vehicle_func.h"
#include "string.h"
#include "animated_tile_func.h"
#include "elrail_func.h"
#include "station_base.h"
#include "roadstop_base.h"
#include "newgrf_railtype.h"
#include "waypoint_base.h"
#include "waypoint_func.h"
#include "pbs.h"
#include "debug.h"
#include "core/random_func.hpp"
#include "company_base.h"
#include "table/airporttile_ids.h"
#include "newgrf_airporttiles.h"
#include "order_backup.h"
#include "newgrf_house.h"
#include "company_gui.h"
#include "linkgraph/linkgraph_base.h"
#include "linkgraph/refresh.h"
#include "widgets/station_widget.h"
#include "signal_func.h"
#include "map/zoneheight.h"
#include "map/road.h"

#include "table/strings.h"

/**
 * Check whether the given tile is a hangar.
 * @param t the tile to of whether it is a hangar.
 * @pre IsStationTile(t)
 * @return true if and only if the tile is a hangar.
 */
bool IsHangar(TileIndex t)
{
	assert(IsStationTile(t));

	/* If the tile isn't an airport there's no chance it's a hangar. */
	if (!IsAirport(t)) return false;

	const Station *st = Station::GetByTile(t);
	const AirportSpec *as = st->airport.GetSpec();

	for (uint i = 0; i < as->nof_depots; i++) {
		if (st->airport.GetHangarTile(i) == t) return true;
	}

	return false;
}

/**
 * Function to check whether the given tile matches some criterion.
 * @param tile the tile to check
 * @return true if it matches, false otherwise
 */
typedef bool (*CMSAMatcher)(TileIndex tile);

/**
 * Counts the numbers of tiles matching a specific type in the area around
 * @param tile the center tile of the 'count area'
 * @param cmp the comparator/matcher (@see CMSAMatcher)
 * @return the number of matching tiles around
 */
static int CountMapSquareAround(TileIndex tile, CMSAMatcher cmp)
{
	int num = 0;

	for (int dx = -3; dx <= 3; dx++) {
		for (int dy = -3; dy <= 3; dy++) {
			TileIndex t = TileAddWrap(tile, dx, dy);
			if (t != INVALID_TILE && cmp(t)) num++;
		}
	}

	return num;
}

/**
 * Check whether the tile is a mine.
 * @param tile the tile to investigate.
 * @return true if and only if the tile is a mine
 */
static bool CMSAMine(TileIndex tile)
{
	/* No industry */
	if (!IsIndustryTile(tile)) return false;

	const Industry *ind = Industry::GetByTile(tile);

	/* No extractive industry */
	if ((GetIndustrySpec(ind->type)->life_type & INDUSTRYLIFE_EXTRACTIVE) == 0) return false;

	for (uint i = 0; i < lengthof(ind->produced_cargo); i++) {
		/* The industry extracts something non-liquid, i.e. no oil or plastic, so it is a mine.
		 * Also the production of passengers and mail is ignored. */
		if (ind->produced_cargo[i] != CT_INVALID &&
				(CargoSpec::Get(ind->produced_cargo[i])->classes & (CC_LIQUID | CC_PASSENGERS | CC_MAIL)) == 0) {
			return true;
		}
	}

	return false;
}

/**
 * Check whether the tile is water.
 * @param tile the tile to investigate.
 * @return true if and only if the tile is a water tile
 */
static bool CMSAWater(TileIndex tile)
{
	return IsPlainWaterTile(tile);
}

/**
 * Check whether the tile is a tree.
 * @param tile the tile to investigate.
 * @return true if and only if the tile is a tree tile
 */
static bool CMSATree(TileIndex tile)
{
	return IsTreeTile(tile);
}

#define M(x) ((x) - STR_SV_STNAME)

enum StationNaming {
	STATIONNAMING_RAIL,
	STATIONNAMING_ROAD,
	STATIONNAMING_AIRPORT,
	STATIONNAMING_OILRIG,
	STATIONNAMING_DOCK,
	STATIONNAMING_HELIPORT,
};

/** Information to handle station action 0 property 24 correctly */
struct StationNameInformation {
	uint32 free_names; ///< Current bitset of free names (we can remove names).
	bool *indtypes;    ///< Array of bools telling whether an industry type has been found.
};

/**
 * Find a station action 0 property 24 station name, or reduce the
 * free_names if needed.
 * @param tile the tile to search
 * @param user_data the StationNameInformation to base the search on
 * @return true if the tile contains an industry that has not given
 *              its name to one of the other stations in town.
 */
static bool FindNearIndustryName(TileIndex tile, void *user_data)
{
	/* All already found industry types */
	StationNameInformation *sni = (StationNameInformation*)user_data;
	if (!IsIndustryTile(tile)) return false;

	/* If the station name is undefined it means that it doesn't name a station */
	IndustryType indtype = GetIndustryType(tile);
	if (GetIndustrySpec(indtype)->station_name == STR_UNDEFINED) return false;

	/* In all cases if an industry that provides a name is found two of
	 * the standard names will be disabled. */
	sni->free_names &= ~(1 << M(STR_SV_STNAME_OILFIELD) | 1 << M(STR_SV_STNAME_MINES));
	return !sni->indtypes[indtype];
}

static StringID GenerateStationName(Station *st, TileIndex tile, StationNaming name_class)
{
	static const uint32 _gen_station_name_bits[] = {
		0,                                       // STATIONNAMING_RAIL
		0,                                       // STATIONNAMING_ROAD
		1U << M(STR_SV_STNAME_AIRPORT),          // STATIONNAMING_AIRPORT
		1U << M(STR_SV_STNAME_OILFIELD),         // STATIONNAMING_OILRIG
		1U << M(STR_SV_STNAME_DOCKS),            // STATIONNAMING_DOCK
		1U << M(STR_SV_STNAME_HELIPORT),         // STATIONNAMING_HELIPORT
	};

	const Town *t = st->town;
	uint32 free_names = UINT32_MAX;

	bool indtypes[NUM_INDUSTRYTYPES];
	memset(indtypes, 0, sizeof(indtypes));

	const Station *s;
	FOR_ALL_STATIONS(s) {
		if (s != st && s->town == t) {
			if (s->indtype != IT_INVALID) {
				indtypes[s->indtype] = true;
				StringID name = GetIndustrySpec(s->indtype)->station_name;
				if (name != STR_UNDEFINED) {
					/* Filter for other industrytypes with the same name */
					for (IndustryType it = 0; it < NUM_INDUSTRYTYPES; it++) {
						const IndustrySpec *indsp = GetIndustrySpec(it);
						if (indsp->enabled && indsp->station_name == name) indtypes[it] = true;
					}
				}
				continue;
			}
			uint str = M(s->string_id);
			if (str <= 0x20) {
				if (str == M(STR_SV_STNAME_FOREST)) {
					str = M(STR_SV_STNAME_WOODS);
				}
				ClrBit(free_names, str);
			}
		}
	}

	TileIndex indtile = tile;
	StationNameInformation sni = { free_names, indtypes };
	if (CircularTileSearch(&indtile, 7, FindNearIndustryName, &sni)) {
		/* An industry has been found nearby */
		IndustryType indtype = GetIndustryType(indtile);
		const IndustrySpec *indsp = GetIndustrySpec(indtype);
		/* STR_NULL means it only disables oil rig/mines */
		if (indsp->station_name != STR_NULL) {
			st->indtype = indtype;
			return STR_SV_STNAME_FALLBACK;
		}
	}

	/* Oil rigs/mines name could be marked not free by looking for a near by industry. */
	free_names = sni.free_names;

	/* check default names */
	uint32 tmp = free_names & _gen_station_name_bits[name_class];
	if (tmp != 0) return STR_SV_STNAME + FindFirstBit(tmp);

	/* check mine? */
	if (HasBit(free_names, M(STR_SV_STNAME_MINES))) {
		if (CountMapSquareAround(tile, CMSAMine) >= 2) {
			return STR_SV_STNAME_MINES;
		}
	}

	/* check close enough to town to get central as name? */
	if (DistanceMax(tile, t->xy) < 8) {
		if (HasBit(free_names, M(STR_SV_STNAME))) return STR_SV_STNAME;

		if (HasBit(free_names, M(STR_SV_STNAME_CENTRAL))) return STR_SV_STNAME_CENTRAL;
	}

	/* Check lakeside */
	if (HasBit(free_names, M(STR_SV_STNAME_LAKESIDE)) &&
			DistanceFromEdge(tile) < 20 &&
			CountMapSquareAround(tile, CMSAWater) >= 5) {
		return STR_SV_STNAME_LAKESIDE;
	}

	/* Check woods */
	if (HasBit(free_names, M(STR_SV_STNAME_WOODS)) && (
				CountMapSquareAround(tile, CMSATree) >= 8 ||
				CountMapSquareAround(tile, IsTileForestIndustry) >= 2)
			) {
		return _settings_game.game_creation.landscape == LT_TROPIC ? STR_SV_STNAME_FOREST : STR_SV_STNAME_WOODS;
	}

	/* check elevation compared to town */
	int z = GetTileZ(tile);
	int z2 = GetTileZ(t->xy);
	if (z < z2) {
		if (HasBit(free_names, M(STR_SV_STNAME_VALLEY))) return STR_SV_STNAME_VALLEY;
	} else if (z > z2) {
		if (HasBit(free_names, M(STR_SV_STNAME_HEIGHTS))) return STR_SV_STNAME_HEIGHTS;
	}

	/* check direction compared to town */
	static const int8 _direction_and_table[] = {
		~( (1 << M(STR_SV_STNAME_WEST))  | (1 << M(STR_SV_STNAME_EAST)) | (1 << M(STR_SV_STNAME_NORTH)) ),
		~( (1 << M(STR_SV_STNAME_SOUTH)) | (1 << M(STR_SV_STNAME_WEST)) | (1 << M(STR_SV_STNAME_NORTH)) ),
		~( (1 << M(STR_SV_STNAME_SOUTH)) | (1 << M(STR_SV_STNAME_EAST)) | (1 << M(STR_SV_STNAME_NORTH)) ),
		~( (1 << M(STR_SV_STNAME_SOUTH)) | (1 << M(STR_SV_STNAME_WEST)) | (1 << M(STR_SV_STNAME_EAST)) ),
	};

	free_names &= _direction_and_table[
		(TileX(tile) < TileX(t->xy)) +
		(TileY(tile) < TileY(t->xy)) * 2];

	tmp = free_names & ((1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 6) | (1 << 7) | (1 << 12) | (1 << 26) | (1 << 27) | (1 << 28) | (1 << 29) | (1 << 30));
	return (tmp == 0) ? STR_SV_STNAME_FALLBACK : (STR_SV_STNAME + FindFirstBit(tmp));
}
#undef M

/**
 * Find the closest deleted station of the current company
 * @param tile the tile to search from.
 * @return the closest station or NULL if too far.
 */
static Station *GetClosestDeletedStation(TileIndex tile)
{
	uint threshold = 8;
	Station *best_station = NULL;
	Station *st;

	FOR_ALL_STATIONS(st) {
		if (!st->IsInUse() && st->owner == _current_company) {
			uint cur_dist = DistanceManhattan(tile, st->xy);

			if (cur_dist < threshold) {
				threshold = cur_dist;
				best_station = st;
			}
		}
	}

	return best_station;
}


void Station::GetTileArea(TileArea *ta, StationType type) const
{
	switch (type) {
		case STATION_RAIL:
			*ta = this->train_station;
			return;

		case STATION_AIRPORT:
			*ta = this->airport;
			return;

		case STATION_TRUCK:
			*ta = this->truck_station;
			return;

		case STATION_BUS:
			*ta = this->bus_station;
			return;

		case STATION_DOCK:
		case STATION_OILRIG:
			*ta = this->dock_area;
			break;

		default: NOT_REACHED();
	}

	ta->w = 1;
	ta->h = 1;
}

/**
 * Update the virtual coords needed to draw the station sign.
 */
void Station::UpdateVirtCoord()
{
	Point pt = RemapCoords2(TileX(this->xy) * TILE_SIZE, TileY(this->xy) * TILE_SIZE);

	pt.y -= 32 * ZOOM_LVL_BASE;
	if ((this->facilities & FACIL_AIRPORT) && this->airport.type == AT_OILRIG) pt.y -= 16 * ZOOM_LVL_BASE;

	SetDParam(0, this->index);
	SetDParam(1, this->facilities);
	this->sign.UpdatePosition(pt.x, pt.y, STR_VIEWPORT_STATION);

	SetWindowDirty(WC_STATION_VIEW, this->index);
}

/** Update the virtual coords needed to draw the station sign for all stations. */
void UpdateAllStationVirtCoords()
{
	BaseStation *st;

	FOR_ALL_BASE_STATIONS(st) {
		st->UpdateVirtCoord();
	}
}

/**
 * Get a mask of the cargo types that the station accepts.
 * @param st Station to query
 * @return the expected mask
 */
static uint GetAcceptanceMask(const Station *st)
{
	uint mask = 0;

	for (CargoID i = 0; i < NUM_CARGO; i++) {
		if (HasBit(st->goods[i].status, GoodsEntry::GES_ACCEPTANCE)) mask |= 1 << i;
	}
	return mask;
}

/**
 * Items contains the two cargo names that are to be accepted or rejected.
 * msg is the string id of the message to display.
 */
static void ShowRejectOrAcceptNews(const Station *st, uint num_items, CargoID *cargo, StringID msg)
{
	for (uint i = 0; i < num_items; i++) {
		SetDParam(i + 1, CargoSpec::Get(cargo[i])->name);
	}

	SetDParam(0, st->index);
	AddNewsItem(msg, NT_ACCEPTANCE, NF_INCOLOUR | NF_SMALL, NR_STATION, st->index);
}

/**
 * Get the cargo types being produced around a tile area.
 * @param area Tile area
 * @param rad Search radius in addition to the given area
 */
CargoArray GetAreaProduction (const TileArea &area, int rad)
{
	CargoArray produced;

	TileArea ta (area);
	ta.expand (rad);

	/* Loop over all tiles to get the produced cargo of
	 * everything except industries */
	TILE_AREA_LOOP(tile, ta) AddProducedCargo(tile, produced);

	/* Loop over the industries. They produce cargo for
	 * anything that is within 'rad' from their bounding
	 * box. As such if you have e.g. a oil well the tile
	 * area loop might not hit an industry tile while
	 * the industry would produce cargo for the station.
	 */
	const Industry *i;
	FOR_ALL_INDUSTRIES(i) {
		if (!ta.Intersects(i->location)) continue;

		for (uint j = 0; j < lengthof(i->produced_cargo); j++) {
			CargoID cargo = i->produced_cargo[j];
			if (cargo != CT_INVALID) produced[cargo]++;
		}
	}

	return produced;
}

/**
 * Get the acceptance of cargoes around a tile area in 1/8.
 * @param area Tile area
 * @param rad Search radius in addition to given area
 * @param always_accepted bitmask of cargo accepted by houses and headquarters; can be NULL
 */
CargoArray GetAreaAcceptance (const TileArea &area, int rad, uint32 *always_accepted)
{
	CargoArray acceptance;
	if (always_accepted != NULL) *always_accepted = 0;

	TileArea ta (area);
	ta.expand (rad);

	TILE_AREA_LOOP(tile, ta) AddAcceptedCargo(tile, acceptance, always_accepted);

	return acceptance;
}

/**
 * Update the acceptance for a station.
 * @param st Station to update
 * @param show_msg controls whether to display a message that acceptance was changed.
 */
void UpdateStationAcceptance(Station *st, bool show_msg)
{
	/* old accepted goods types */
	uint old_acc = GetAcceptanceMask(st);

	/* And retrieve the acceptance. */
	CargoArray acceptance;
	if (!st->rect.empty()) {
		acceptance = GetAreaAcceptance (st->rect,
			st->GetCatchmentRadius(), &st->always_accepted);
	}

	/* Adjust in case our station only accepts fewer kinds of goods */
	for (CargoID i = 0; i < NUM_CARGO; i++) {
		/* Make sure the station can accept the goods type. */
		uint amt = st->CanHandleCargo(i) ? acceptance[i] : 0;

		GoodsEntry &ge = st->goods[i];
		SB(ge.status, GoodsEntry::GES_ACCEPTANCE, 1, amt >= 8);
		if (LinkGraph::IsValidID(ge.link_graph)) {
			(*LinkGraph::Get(ge.link_graph))[ge.node].SetDemand(amt / 8);
		}
	}

	/* Only show a message in case the acceptance was actually changed. */
	uint new_acc = GetAcceptanceMask(st);
	if (old_acc == new_acc) return;

	/* show a message to report that the acceptance was changed? */
	if (show_msg && st->owner == _local_company && st->IsInUse()) {
		/* List of accept and reject strings for different number of
		 * cargo types */
		static const StringID accept_msg[] = {
			STR_NEWS_STATION_NOW_ACCEPTS_CARGO,
			STR_NEWS_STATION_NOW_ACCEPTS_CARGO_AND_CARGO,
		};
		static const StringID reject_msg[] = {
			STR_NEWS_STATION_NO_LONGER_ACCEPTS_CARGO,
			STR_NEWS_STATION_NO_LONGER_ACCEPTS_CARGO_OR_CARGO,
		};

		/* Array of accepted and rejected cargo types */
		CargoID accepts[2] = { CT_INVALID, CT_INVALID };
		CargoID rejects[2] = { CT_INVALID, CT_INVALID };
		uint num_acc = 0;
		uint num_rej = 0;

		/* Test each cargo type to see if its acceptance has changed */
		for (CargoID i = 0; i < NUM_CARGO; i++) {
			if (HasBit(new_acc, i)) {
				if (!HasBit(old_acc, i) && num_acc < lengthof(accepts)) {
					/* New cargo is accepted */
					accepts[num_acc++] = i;
				}
			} else {
				if (HasBit(old_acc, i) && num_rej < lengthof(rejects)) {
					/* Old cargo is no longer accepted */
					rejects[num_rej++] = i;
				}
			}
		}

		/* Show news message if there are any changes */
		if (num_acc > 0) ShowRejectOrAcceptNews(st, num_acc, accepts, accept_msg[num_acc - 1]);
		if (num_rej > 0) ShowRejectOrAcceptNews(st, num_rej, rejects, reject_msg[num_rej - 1]);
	}

	/* redraw the station view since acceptance changed */
	SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_ACCEPT_RATING_LIST);
}

static void UpdateStationSignCoord(BaseStation *st)
{
	const TileArea *r = &st->rect;

	if (r->empty()) return; // no tiles belong to this station

	/* clamp sign coord to be inside the station rect */
	st->xy = st->rect.get_closest_tile(st->xy);
	st->UpdateVirtCoord();

	if (st->IsWaypoint()) return;
	Station *full_station = Station::From(st);
	for (CargoID c = 0; c < NUM_CARGO; ++c) {
		LinkGraphID lg = full_station->goods[c].link_graph;
		if (!LinkGraph::IsValidID(lg)) continue;
		LinkGraph::Get(lg)->UpdateDistances(full_station->goods[c].node, st->xy);
	}
}

/**
 * Common part of building various station parts and possibly attaching them to an existing one.
 * @param [in,out] st Station to attach to
 * @param flags Command flags
 * @param reuse Whether to try to reuse a deleted station (gray sign) if possible
 * @param area Area occupied by the new part
 * @param name_class Station naming class to use to generate the new station's name
 * @return Command error that occurred, if any
 */
static CommandCost BuildStationPart(Station **st, DoCommandFlag flags, bool reuse, TileArea area, StationNaming name_class)
{
	/* Find a deleted station close to us */
	if (*st == NULL && reuse) *st = GetClosestDeletedStation(area.tile);

	if (*st != NULL) {
		if ((*st)->owner != _current_company) {
			return_cmd_error(STR_ERROR_TOO_CLOSE_TO_ANOTHER_STATION);
		}

		if (!(*st)->TestAddRect(area)) {
			return_cmd_error(STR_ERROR_STATION_TOO_SPREAD_OUT);
		}
	} else {
		/* allocate and initialize new station */
		if (!Station::CanAllocateItem()) return_cmd_error(STR_ERROR_TOO_MANY_STATIONS_LOADING);

		if (flags & DC_EXEC) {
			*st = new Station(area.tile);

			(*st)->town = ClosestTownFromTile(area.tile);
			(*st)->string_id = GenerateStationName(*st, area.tile, name_class);

			if (Company::IsValidID(_current_company)) {
				SetBit((*st)->town->have_ratings, _current_company);
			}
		}
	}
	return CommandCost();
}

/**
 * This is called right after a station was deleted.
 * It checks if the whole station is free of substations, and if so, the station will be
 * deleted after a little while.
 * @param st Station
 */
static void DeleteStationIfEmpty(BaseStation *st)
{
	if (!st->IsInUse()) {
		st->delete_ctr = 0;
		InvalidateWindowData(WC_STATION_LIST, st->owner, 0);
	}
	/* station remains but it probably lost some parts - station sign should stay in the station boundaries */
	UpdateStationSignCoord(st);
}

CommandCost ClearTile_Station(TileIndex tile, DoCommandFlag flags);

/**
 * Checks if the given tile is buildable, flat and has a certain height.
 * @param tile TileIndex to check.
 * @param invalid_dirs Prohibited directions for slopes (set of #DiagDirection).
 * @param allowed_z Height allowed for the tile. If allowed_z is negative, it will be set to the height of this tile.
 * @param allow_steep Whether steep slopes are allowed.
 * @param check_bridge Check for the existence of a bridge.
 * @return The cost in case of success, or an error code if it failed.
 */
CommandCost CheckBuildableTile(TileIndex tile, uint invalid_dirs, int &allowed_z, bool allow_steep, bool check_bridge = true)
{
	if (check_bridge && HasBridgeAbove(tile)) {
		return_cmd_error(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);
	}

	CommandCost ret = EnsureNoVehicleOnGround(tile);
	if (ret.Failed()) return ret;

	int z;
	Slope tileh = GetTileSlope(tile, &z);

	/* Prohibit building if
	 *   1) The tile is "steep" (i.e. stretches two height levels).
	 *   2) The tile is non-flat and the build_on_slopes switch is disabled.
	 */
	if ((!allow_steep && IsSteepSlope(tileh)) ||
			((!_settings_game.construction.build_on_slopes) && tileh != SLOPE_FLAT)) {
		return_cmd_error(STR_ERROR_FLAT_LAND_REQUIRED);
	}

	CommandCost cost(EXPENSES_CONSTRUCTION);
	int flat_z = z + GetSlopeMaxZ(tileh);
	if (tileh != SLOPE_FLAT) {
		/* Forbid building if the tile faces a slope in a invalid direction. */
		for (DiagDirection dir = DIAGDIR_BEGIN; dir != DIAGDIR_END; dir++) {
			if (HasBit(invalid_dirs, dir) && !CanBuildDepotByTileh(dir, tileh)) {
				return_cmd_error(STR_ERROR_FLAT_LAND_REQUIRED);
			}
		}
		cost.AddCost(_price[PR_BUILD_FOUNDATION]);
	}

	/* The level of this tile must be equal to allowed_z. */
	if (allowed_z < 0) {
		/* First tile. */
		allowed_z = flat_z;
	} else if (allowed_z != flat_z) {
		return_cmd_error(STR_ERROR_FLAT_LAND_REQUIRED);
	}

	return cost;
}

/**
 * Tries to clear the given area.
 * @param tile_area Area to check.
 * @param flags Operation to perform.
 * @return The cost in case of success, or an error code if it failed.
 */
CommandCost CheckFlatLand(TileArea tile_area, DoCommandFlag flags)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	int allowed_z = -1;

	TILE_AREA_LOOP(tile_cur, tile_area) {
		CommandCost ret = CheckBuildableTile(tile_cur, 0, allowed_z, true);
		if (ret.Failed()) return ret;
		cost.AddCost(ret);

		ret = DoCommand(tile_cur, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
		if (ret.Failed()) return ret;
		cost.AddCost(ret);
	}

	return cost;
}

/**
 * Checks if a rail station can be built at the given area.
 * @param tile_area Area to check.
 * @param flags Operation to perform.
 * @param axis Rail station axis.
 * @param station StationID to be queried and returned if available.
 * @param rt The rail type to check for (overbuilding rail stations over rail).
 * @param affected_vehicles List of trains with PBS reservations on the tiles
 * @param spec_class Station class.
 * @param spec_index Index into the station class.
 * @param plat_len Platform length.
 * @param numtracks Number of platforms.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost CheckFlatLandRailStation(TileArea tile_area, DoCommandFlag flags, Axis axis, StationID *station, RailType rt, SmallVector<Train *, 4> &affected_vehicles, StationClassID spec_class, byte spec_index, byte plat_len, byte numtracks)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	int allowed_z = -1;
	uint invalid_dirs = 5 << axis;

	const StationSpec *statspec = StationClass::Get(spec_class)->GetSpec(spec_index);
	bool slope_cb = statspec != NULL && HasBit(statspec->callback_mask, CBM_STATION_SLOPE_CHECK);

	TILE_AREA_LOOP(tile_cur, tile_area) {
		CommandCost ret = CheckBuildableTile(tile_cur, invalid_dirs, allowed_z, false);
		if (ret.Failed()) return ret;
		cost.AddCost(ret);

		if (slope_cb) {
			/* Do slope check if requested. */
			ret = PerformStationTileSlopeCheck(tile_area.tile, tile_cur, statspec, axis, plat_len, numtracks);
			if (ret.Failed()) return ret;
		}

		/* if station is set, then we have special handling to allow building on top of already existing stations.
		 * so station points to INVALID_STATION if we can build on any station.
		 * Or it points to a station if we're only allowed to build on exactly that station. */
		if (station != NULL && IsStationTile(tile_cur)) {
			if (!IsRailStation(tile_cur)) {
				return ClearTile_Station(tile_cur, DC_AUTO); // get error message
			} else {
				StationID st = GetStationIndex(tile_cur);
				if (*station == INVALID_STATION) {
					*station = st;
				} else if (*station != st) {
					return_cmd_error(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
				}
			}
		} else {
			/* Rail type is only valid when building a railway station; if station to
			 * build isn't a rail station it's INVALID_RAILTYPE. */
			if (rt != INVALID_RAILTYPE && IsNormalRailTile(tile_cur) &&
					HasPowerOnRail(GetRailType(tile_cur), rt)) {
				/* Allow overbuilding if the tile:
				 *  - has rail, but no signals
				 *  - it has exactly one track
				 *  - the track is in line with the station
				 *  - the current rail type has power on the to-be-built type (e.g. convert normal rail to el rail)
				 */
				Track track = AxisToTrack(axis);

				if (GetTrackBits(tile_cur) == TrackToTrackBits(track) && !HasSignalOnTrack(tile_cur, track)) {
					/* Check for trains having a reservation for this tile. */
					if (HasBit(GetRailReservationTrackBits(tile_cur), track)) {
						Train *v = GetTrainForReservation(tile_cur, track);
						if (v != NULL) {
							*affected_vehicles.Append() = v;
						}
					}
					CommandCost ret = DoCommand(tile_cur, 0, track, flags, CMD_REMOVE_SINGLE_RAIL);
					if (ret.Failed()) return ret;
					cost.AddCost(ret);
					/* With flags & ~DC_EXEC CmdLandscapeClear would fail since the rail still exists */
					continue;
				}
			}
			ret = DoCommand(tile_cur, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
			if (ret.Failed()) return ret;
			cost.AddCost(ret);
		}
	}

	return cost;
}

/**
 * Checks if a road stop can be built at the given tile.
 * @param tile_area Area to check.
 * @param flags Operation to perform.
 * @param invalid_dirs Prohibited directions (set of DiagDirections).
 * @param is_drive_through True if trying to build a drive-through station.
 * @param is_truck_stop True when building a truck stop, false otherwise.
 * @param axis Axis of a drive-through road stop.
 * @param station StationID to be queried and returned if available.
 * @param rts Road types to build.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost CheckFlatLandRoadStop(TileArea tile_area, DoCommandFlag flags, uint invalid_dirs, bool is_drive_through, bool is_truck_stop, Axis axis, StationID *station, RoadTypes rts)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	int allowed_z = -1;

	TILE_AREA_LOOP(cur_tile, tile_area) {
		CommandCost ret = CheckBuildableTile(cur_tile, invalid_dirs, allowed_z, !is_drive_through);
		if (ret.Failed()) return ret;
		cost.AddCost(ret);

		/* If station is set, then we have special handling to allow building on top of already existing stations.
		 * Station points to INVALID_STATION if we can build on any station.
		 * Or it points to a station if we're only allowed to build on exactly that station. */
		if (station != NULL && IsStationTile(cur_tile)) {
			if (!IsRoadStop(cur_tile)) {
				return ClearTile_Station(cur_tile, DC_AUTO); // Get error message.
			} else {
				if (is_truck_stop != IsTruckStop(cur_tile) ||
						is_drive_through != IsDriveThroughStopTile(cur_tile)) {
					return ClearTile_Station(cur_tile, DC_AUTO); // Get error message.
				}
				/* Drive-through station in the wrong direction. */
				if (is_drive_through && IsDriveThroughStopTile(cur_tile) && GetRoadStopAxis(cur_tile) != axis){
					return_cmd_error(STR_ERROR_DRIVE_THROUGH_DIRECTION);
				}
				StationID st = GetStationIndex(cur_tile);
				if (*station == INVALID_STATION) {
					*station = st;
				} else if (*station != st) {
					return_cmd_error(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
				}
			}
		} else {
			bool build_over_road = is_drive_through && IsNormalRoadTile(cur_tile);
			/* Road bits in the wrong direction. */
			RoadBits rb = IsRoadTile(cur_tile) ? GetAllRoadBits(cur_tile) : ROAD_NONE;
			if (build_over_road && (rb & (axis == AXIS_X ? ROAD_Y : ROAD_X)) != 0) {
				/* Someone was pedantic and *NEEDED* three fracking different error messages. */
				switch (CountBits(rb)) {
					case 1:
						return_cmd_error(STR_ERROR_DRIVE_THROUGH_DIRECTION);

					case 2:
						if (rb == ROAD_X || rb == ROAD_Y) return_cmd_error(STR_ERROR_DRIVE_THROUGH_DIRECTION);
						return_cmd_error(STR_ERROR_DRIVE_THROUGH_CORNER);

					default: // 3 or 4
						return_cmd_error(STR_ERROR_DRIVE_THROUGH_JUNCTION);
				}
			}

			RoadTypes cur_rts = IsRoadTile(cur_tile) ? GetRoadTypes(cur_tile) : ROADTYPES_NONE;
			uint num_roadbits = 0;
			if (build_over_road) {
				/* There is a road, check if we can build road+tram stop over it. */
				if (HasBit(cur_rts, ROADTYPE_ROAD)) {
					Owner road_owner = GetRoadOwner(cur_tile, ROADTYPE_ROAD);
					if (road_owner == OWNER_TOWN) {
						if (!_settings_game.construction.road_stop_on_town_road) return_cmd_error(STR_ERROR_DRIVE_THROUGH_ON_TOWN_ROAD);
					} else if (!_settings_game.construction.road_stop_on_competitor_road && road_owner != OWNER_NONE) {
						CommandCost ret = CheckOwnership(road_owner);
						if (ret.Failed()) return ret;
					}
					num_roadbits += CountBits(GetRoadBits(cur_tile, ROADTYPE_ROAD));
				}

				/* There is a tram, check if we can build road+tram stop over it. */
				if (HasBit(cur_rts, ROADTYPE_TRAM)) {
					Owner tram_owner = GetRoadOwner(cur_tile, ROADTYPE_TRAM);
					if (!_settings_game.construction.road_stop_on_competitor_road && tram_owner != OWNER_NONE) {
						CommandCost ret = CheckOwnership(tram_owner);
						if (ret.Failed()) return ret;
					}
					num_roadbits += CountBits(GetRoadBits(cur_tile, ROADTYPE_TRAM));
				}

				/* Take into account existing roadbits. */
				rts |= cur_rts;
			} else {
				ret = DoCommand(cur_tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
				if (ret.Failed()) return ret;
				cost.AddCost(ret);
			}

			uint roadbits_to_build = CountBits(rts) * 2 - num_roadbits;
			cost.AddCost(_price[PR_BUILD_ROAD] * roadbits_to_build);
		}
	}

	return cost;
}

/**
 * Checks if an airport can be built at the given area.
 * @param tile_area Area to check.
 * @param flags Operation to perform.
 * @param station StationID of airport allowed in search area.
 * @return The cost in case of success, or an error code if it failed.
 */
static CommandCost CheckFlatLandAirport(TileArea tile_area, DoCommandFlag flags, StationID *station)
{
	CommandCost cost(EXPENSES_CONSTRUCTION);
	int allowed_z = -1;

	TILE_AREA_LOOP(tile_cur, tile_area) {
		CommandCost ret = CheckBuildableTile(tile_cur, 0, allowed_z, true);
		if (ret.Failed()) return ret;
		cost.AddCost(ret);

		/* if station is set, then allow building on top of an already
		 * existing airport, either the one in *station if it is not
		 * INVALID_STATION, or anyone otherwise and store which one
		 * in *station */
		if (station != NULL && IsStationTile(tile_cur)) {
			if (!IsAirport(tile_cur)) {
				return ClearTile_Station(tile_cur, DC_AUTO); // get error message
			} else {
				StationID st = GetStationIndex(tile_cur);
				if (*station == INVALID_STATION) {
					*station = st;
				} else if (*station != st) {
					return_cmd_error(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
				}
			}
		} else {
			ret = DoCommand(tile_cur, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
			if (ret.Failed()) return ret;
			cost.AddCost(ret);
		}
	}

	return cost;
}

/**
 * Check whether we can expand the rail part of the given station.
 * @param st the station to expand
 * @param new_ta the current (and if all is fine new) tile area of the rail part of the station
 * @param axis the axis of the newly build rail
 * @return Succeeded or failed command.
 */
CommandCost CanExpandRailStation(const BaseStation *st, TileArea &new_ta, Axis axis)
{
	TileArea cur_ta = st->train_station;

	/* determine new size of train station region.. */
	int x = min(TileX(cur_ta.tile), TileX(new_ta.tile));
	int y = min(TileY(cur_ta.tile), TileY(new_ta.tile));
	new_ta.w = max(TileX(cur_ta.tile) + cur_ta.w, TileX(new_ta.tile) + new_ta.w) - x;
	new_ta.h = max(TileY(cur_ta.tile) + cur_ta.h, TileY(new_ta.tile) + new_ta.h) - y;
	new_ta.tile = TileXY(x, y);

	/* make sure the final size is not too big. */
	if (new_ta.w > _settings_game.station.station_spread || new_ta.h > _settings_game.station.station_spread) {
		return_cmd_error(STR_ERROR_STATION_TOO_SPREAD_OUT);
	}

	return CommandCost();
}

static inline byte *CreateSingle(byte *layout, int n)
{
	int i = n;
	do *layout++ = 0; while (--i);
	layout[((n - 1) >> 1) - n] = 2;
	return layout;
}

static inline byte *CreateMulti(byte *layout, int n, byte b)
{
	int i = n;
	do *layout++ = b; while (--i);
	if (n > 4) {
		layout[0 - n] = 0;
		layout[n - 1 - n] = 0;
	}
	return layout;
}

/**
 * Create the station layout for the given number of tracks and platform length.
 * @param layout    The layout to write to.
 * @param numtracks The number of tracks to write.
 * @param plat_len  The length of the platforms.
 * @param statspec  The specification of the station to (possibly) get the layout from.
 */
void GetStationLayout(byte *layout, int numtracks, int plat_len, const StationSpec *statspec)
{
	if (statspec != NULL && statspec->lengths >= plat_len &&
			statspec->platforms[plat_len - 1] >= numtracks &&
			statspec->layouts[plat_len - 1][numtracks - 1]) {
		/* Custom layout defined, follow it. */
		memcpy(layout, statspec->layouts[plat_len - 1][numtracks - 1],
			plat_len * numtracks);
		return;
	}

	if (plat_len == 1) {
		CreateSingle(layout, numtracks);
	} else {
		if (numtracks & 1) layout = CreateSingle(layout, plat_len);
		numtracks >>= 1;

		while (--numtracks >= 0) {
			layout = CreateMulti(layout, plat_len, 4);
			layout = CreateMulti(layout, plat_len, 6);
		}
	}
}

/**
 * Find a nearby station that joins this station.
 * @tparam T the class to find a station for
 * @param existing_station an existing station we build over
 * @param station_to_join the station to join to
 * @param adjacent whether adjacent stations are allowed
 * @param ta the area of the newly build station
 * @param st 'return' pointer for the found station
 * @param error_message the error message when building a station on top of others
 * @return command cost with the error or 'okay'
 */
template <class T>
CommandCost FindJoiningBaseStation(StationID existing_station, StationID station_to_join, bool adjacent, TileArea ta, T **st, StringID error_message)
{
	assert(*st == NULL);

	bool check_surrounding;
	if (!_settings_game.station.adjacent_stations) {
		check_surrounding = true;
	} else if (existing_station == INVALID_STATION) {
		/* There's no station here. Don't check the tiles surrounding this
		 * one if the company wanted to build an adjacent station. */
		check_surrounding = !adjacent;
	} else if (adjacent && existing_station != station_to_join) {
		/* You can't build an adjacent station over the top of one that
		 * already exists. */
		return_cmd_error(error_message);
	} else {
		/* Extend the current station, and don't check whether it will
		 * be near any other stations. */
		*st = T::GetIfValid(existing_station);
		check_surrounding = (*st == NULL);
	}

	if (check_surrounding) {
		/* Make sure there are no similar stations around us. */
		ta.tile -= TileDiffXY(1, 1);
		ta.w    += 2;
		ta.h    += 2;

		/* check around to see if there are any stations there */
		TILE_AREA_LOOP(tile_cur, ta) {
			if (IsStationTile(tile_cur)) {
				StationID t = GetStationIndex(tile_cur);
				if (!T::IsValidID(t)) continue;

				if (existing_station == INVALID_STATION) {
					existing_station = t;
				} else if (existing_station != t) {
					return_cmd_error(STR_ERROR_ADJOINS_MORE_THAN_ONE_EXISTING);
				}
			}
		}

		*st = (existing_station == INVALID_STATION) ? NULL : T::Get(existing_station);
	}

	/* Distant join */
	if (*st == NULL && station_to_join != INVALID_STATION) *st = T::GetIfValid(station_to_join);

	return CommandCost();
}

/**
 * Find a nearby station that joins this station.
 * @param existing_station an existing station we build over
 * @param station_to_join the station to join to
 * @param adjacent whether adjacent stations are allowed
 * @param ta the area of the newly build station
 * @param st 'return' pointer for the found station
 * @param error_message the error message when building a station on top of others
 * @return command cost with the error or 'okay'
 */
static CommandCost FindJoiningStation(StationID existing_station, StationID station_to_join, bool adjacent, TileArea ta, Station **st, StringID error_message = STR_ERROR_MUST_REMOVE_RAILWAY_STATION_FIRST)
{
	return FindJoiningBaseStation<Station>(existing_station, station_to_join, adjacent, ta, st, error_message);
}

/**
 * Find a nearby waypoint that joins this waypoint.
 * @param existing_waypoint an existing waypoint we build over
 * @param waypoint_to_join the waypoint to join to
 * @param adjacent whether adjacent waypoints are allowed
 * @param ta the area of the newly build waypoint
 * @param wp 'return' pointer for the found waypoint
 * @return command cost with the error or 'okay'
 */
CommandCost FindJoiningWaypoint(StationID existing_waypoint, StationID waypoint_to_join, bool adjacent, TileArea ta, Waypoint **wp)
{
	return FindJoiningBaseStation<Waypoint>(existing_waypoint, waypoint_to_join, adjacent, ta, wp, STR_ERROR_MUST_REMOVE_RAILWAYPOINT_FIRST);
}

static void FreeTrainReservation(Train *v)
{
	FreeTrainTrackReservation(v);

	const RailPathPos pos = v->GetPos();
	if (!pos.in_wormhole() && IsRailStationTile(pos.tile)) SetRailStationPlatformReservation(pos, false);

	const RailPathPos rev = v->Last()->GetReversePos();
	if (!rev.in_wormhole() && IsRailStationTile(rev.tile)) SetRailStationPlatformReservation(rev, false);
}

static void RestoreTrainReservation(Train *v)
{
	const RailPathPos pos = v->GetPos();
	if (!pos.in_wormhole() && IsRailStationTile(pos.tile)) SetRailStationPlatformReservation(pos, true);

	TryPathReserve(v, true, true);

	const RailPathPos rev = v->Last()->GetReversePos();
	if (!rev.in_wormhole() && IsRailStationTile(rev.tile)) SetRailStationPlatformReservation(rev, true);
}

/**
 * Build rail station
 * @param tile_org northern most position of station dragging/placement
 * @param flags operation to perform
 * @param p1 various bitstuffed elements
 * - p1 = (bit  0- 3) - railtype
 * - p1 = (bit  4)    - orientation (Axis)
 * - p1 = (bit  8-15) - number of tracks
 * - p1 = (bit 16-23) - platform length
 * - p1 = (bit 24)    - allow stations directly adjacent to other stations.
 * @param p2 various bitstuffed elements
 * - p2 = (bit  0- 7) - custom station class
 * - p2 = (bit  8-15) - custom station id
 * - p2 = (bit 16-31) - station ID to join (NEW_STATION if build new one)
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildRailStation(TileIndex tile_org, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	/* Unpack parameters */
	RailType rt    = Extract<RailType, 0, 4>(p1);
	Axis axis      = Extract<Axis, 4, 1>(p1);
	byte numtracks = GB(p1,  8, 8);
	byte plat_len  = GB(p1, 16, 8);
	bool adjacent  = HasBit(p1, 24);

	StationClassID spec_class = Extract<StationClassID, 0, 8>(p2);
	byte spec_index           = GB(p2, 8, 8);
	StationID station_to_join = GB(p2, 16, 16);

	/* Does the authority allow this? */
	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile_org, flags);
	if (ret.Failed()) return ret;

	if (!ValParamRailtype(rt)) return CMD_ERROR;

	/* Check if the given station class is valid */
	if ((uint)spec_class >= StationClass::GetClassCount() || spec_class == STAT_CLASS_WAYP) return CMD_ERROR;
	if (spec_index >= StationClass::Get(spec_class)->GetSpecCount()) return CMD_ERROR;
	if (plat_len == 0 || numtracks == 0) return CMD_ERROR;

	int w_org, h_org;
	if (axis == AXIS_X) {
		w_org = plat_len;
		h_org = numtracks;
	} else {
		h_org = plat_len;
		w_org = numtracks;
	}

	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = INVALID_STATION;
	bool distant_join = (station_to_join != INVALID_STATION);

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (h_org > _settings_game.station.station_spread || w_org > _settings_game.station.station_spread) return CMD_ERROR;

	/* these values are those that will be stored in train_tile and station_platforms */
	TileArea new_location(tile_org, w_org, h_org);

	/* Make sure the area below consists of clear tiles. (OR tiles belonging to a certain rail station) */
	StationID est = INVALID_STATION;
	SmallVector<Train *, 4> affected_vehicles;
	/* Clear the land below the station. */
	CommandCost cost = CheckFlatLandRailStation(new_location, flags, axis, &est, rt, affected_vehicles, spec_class, spec_index, plat_len, numtracks);
	if (cost.Failed()) return cost;
	/* Add construction expenses. */
	cost.AddCost((numtracks * _price[PR_BUILD_STATION_RAIL] + _price[PR_BUILD_STATION_RAIL_LENGTH]) * plat_len);
	cost.AddCost(numtracks * plat_len * RailBuildCost(rt));

	Station *st = NULL;
	ret = FindJoiningStation(est, station_to_join, adjacent, new_location, &st);
	if (ret.Failed()) return ret;

	ret = BuildStationPart(&st, flags, reuse, new_location, STATIONNAMING_RAIL);
	if (ret.Failed()) return ret;

	if (st != NULL && st->train_station.tile != INVALID_TILE) {
		CommandCost ret = CanExpandRailStation(st, new_location, axis);
		if (ret.Failed()) return ret;
	}

	/* Check if we can allocate a custom stationspec to this station */
	const StationSpec *statspec = StationClass::Get(spec_class)->GetSpec(spec_index);
	int specindex = AllocateSpecToStation(statspec, st, (flags & DC_EXEC) != 0);
	if (specindex == -1) return_cmd_error(STR_ERROR_TOO_MANY_STATION_SPECS);

	if (statspec != NULL) {
		/* Perform NewStation checks */

		/* Check if the station size is permitted */
		if (HasBit(statspec->disallowed_platforms, min(numtracks - 1, 7)) || HasBit(statspec->disallowed_lengths, min(plat_len - 1, 7))) {
			return CMD_ERROR;
		}

		/* Check if the station is buildable */
		if (HasBit(statspec->callback_mask, CBM_STATION_AVAIL)) {
			uint16 cb_res = GetStationCallback(CBID_STATION_AVAILABILITY, 0, 0, statspec, NULL, INVALID_TILE);
			if (cb_res != CALLBACK_FAILED && !Convert8bitBooleanCallback(statspec->grf_prop.grffile, CBID_STATION_AVAILABILITY, cb_res)) return CMD_ERROR;
		}
	}

	if (flags & DC_EXEC) {
		TileIndexDiff tile_delta;
		byte *layout_ptr;
		byte numtracks_orig;
		Track track;

		st->train_station = new_location;
		st->AddFacility(FACIL_TRAIN, new_location.tile);

		st->rect.Add (TileArea (tile_org, w_org, h_org));

		if (statspec != NULL) {
			/* Include this station spec's animation trigger bitmask
			 * in the station's cached copy. */
			st->cached_anim_triggers |= statspec->animation.triggers;
		}

		tile_delta = (axis == AXIS_X ? TileDiffXY(1, 0) : TileDiffXY(0, 1));
		track = AxisToTrack(axis);

		layout_ptr = AllocaM(byte, numtracks * plat_len);
		GetStationLayout(layout_ptr, numtracks, plat_len, statspec);

		numtracks_orig = numtracks;

		Company *c = Company::Get(st->owner);
		TileIndex tile_track = tile_org;
		do {
			TileIndex tile = tile_track;
			int w = plat_len;
			do {
				byte layout = *layout_ptr++;
				if (IsRailStationTile(tile) && HasStationReservation(tile)) {
					/* Check for trains having a reservation for this tile. */
					Train *v = GetTrainForReservation(tile, AxisToTrack(GetRailStationAxis(tile)));
					if (v != NULL) {
						*affected_vehicles.Append() = v;
						FreeTrainReservation(v);
					}
				}

				/* Railtype can change when overbuilding. */
				if (IsRailStationTile(tile)) {
					if (!IsStationTileBlocked(tile)) c->infrastructure.rail[GetRailType(tile)]--;
					c->infrastructure.station--;
				}

				/* Remove animation if overbuilding */
				DeleteAnimatedTile(tile);
				byte old_specindex = HasStationTileRail(tile) ? GetCustomStationSpecIndex(tile) : 0;
				MakeRailStation(tile, st->owner, st->index, axis, layout & ~1, rt);
				/* Free the spec if we overbuild something */
				DeallocateSpecFromStation(st, old_specindex);

				SetCustomStationSpecIndex(tile, specindex);
				SetStationTileRandomBits(tile, GB(Random(), 0, 4));
				SetAnimationFrame(tile, 0);

				if (!IsStationTileBlocked(tile)) c->infrastructure.rail[rt]++;
				c->infrastructure.station++;

				if (statspec != NULL) {
					/* Use a fixed axis for GetPlatformInfo as our platforms / numtracks are always the right way around */
					uint32 platinfo = GetPlatformInfo(AXIS_X, GetStationGfx(tile), plat_len, numtracks_orig, plat_len - w, numtracks_orig - numtracks, false);

					/* As the station is not yet completely finished, the station does not yet exist. */
					uint16 callback = GetStationCallback(CBID_STATION_TILE_LAYOUT, platinfo, 0, statspec, NULL, tile);
					if (callback != CALLBACK_FAILED) {
						if (callback < 8) {
							SetStationGfx(tile, (callback & ~1) + axis);
						} else {
							ErrorUnknownCallbackResult(statspec->grf_prop.grffile->grfid, CBID_STATION_TILE_LAYOUT, callback);
						}
					}

					/* Trigger station animation -- after building? */
					TriggerStationAnimation(st, tile, SAT_BUILT);
				}

				tile += tile_delta;
			} while (--w);
			AddTrackToSignalBuffer(tile_track, track, _current_company);
			YapfNotifyTrackLayoutChange(tile_track, track);
			tile_track += tile_delta ^ TileDiffXY(1, 1); // perpendicular to tile_delta
		} while (--numtracks);

		for (uint i = 0; i < affected_vehicles.Length(); ++i) {
			RestoreTrainReservation(affected_vehicles[i]);
		}

		/* Check whether we need to expand the reservation of trains already on the station. */
		TileArea update_reservation_area;
		if (axis == AXIS_X) {
			update_reservation_area = TileArea(tile_org, 1, numtracks_orig);
		} else {
			update_reservation_area = TileArea(tile_org, numtracks_orig, 1);
		}

		TILE_AREA_LOOP(tile, update_reservation_area) {
			/* Don't even try to make eye candy parts reserved. */
			if (IsStationTileBlocked(tile)) continue;

			DiagDirection dir = AxisToDiagDir(axis);
			TileIndexDiff tile_offset = TileOffsByDiagDir(dir);
			TileIndex platform_begin = tile;
			TileIndex platform_end = tile;

			/* We can only account for tiles that are reachable from this tile, so ignore primarily blocked tiles while finding the platform begin and end. */
			for (TileIndex next_tile = platform_begin - tile_offset; IsCompatibleTrainStationTile(next_tile, platform_begin); next_tile -= tile_offset) {
				platform_begin = next_tile;
			}
			for (TileIndex next_tile = platform_end + tile_offset; IsCompatibleTrainStationTile(next_tile, platform_end); next_tile += tile_offset) {
				platform_end = next_tile;
			}

			/* If there is at least on reservation on the platform, we reserve the whole platform. */
			bool reservation = false;
			for (TileIndex t = platform_begin; !reservation && t <= platform_end; t += tile_offset) {
				reservation = HasStationReservation(t);
			}

			if (reservation) {
				SetRailStationPlatformReservation(platform_begin, dir, true);
			}
		}

		st->MarkTilesDirty(false);
		st->UpdateVirtCoord();
		UpdateStationAcceptance(st, false);
		st->RecomputeIndustriesNear();
		InvalidateWindowData(WC_SELECT_STATION, 0, 0);
		InvalidateWindowData(WC_STATION_LIST, st->owner, 0);
		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_TRAINS);
		DirtyCompanyInfrastructureWindows(st->owner);
	}

	return cost;
}

static void MakeRailStationAreaSmaller(BaseStation *st)
{
	TileArea ta = st->train_station;

restart:

	/* too small? */
	if (ta.w != 0 && ta.h != 0) {
		/* check the left side, x = constant, y changes */
		for (uint i = 0; !st->TileBelongsToRailStation(ta.tile + TileDiffXY(0, i));) {
			/* the left side is unused? */
			if (++i == ta.h) {
				ta.tile += TileDiffXY(1, 0);
				ta.w--;
				goto restart;
			}
		}

		/* check the right side, x = constant, y changes */
		for (uint i = 0; !st->TileBelongsToRailStation(ta.tile + TileDiffXY(ta.w - 1, i));) {
			/* the right side is unused? */
			if (++i == ta.h) {
				ta.w--;
				goto restart;
			}
		}

		/* check the upper side, y = constant, x changes */
		for (uint i = 0; !st->TileBelongsToRailStation(ta.tile + TileDiffXY(i, 0));) {
			/* the left side is unused? */
			if (++i == ta.w) {
				ta.tile += TileDiffXY(0, 1);
				ta.h--;
				goto restart;
			}
		}

		/* check the lower side, y = constant, x changes */
		for (uint i = 0; !st->TileBelongsToRailStation(ta.tile + TileDiffXY(i, ta.h - 1));) {
			/* the left side is unused? */
			if (++i == ta.w) {
				ta.h--;
				goto restart;
			}
		}
	} else {
		ta.Clear();
	}

	st->train_station = ta;
}

/**
 * Remove a number of tiles from any rail station within the area.
 * @param ta the area to clear station tile from.
 * @param affected_stations the stations affected.
 * @param flags the command flags.
 * @param removal_cost the cost for removing the tile, including the rail.
 * @param keep_rail whether to keep the rail of the station.
 * @tparam T the type of station to remove.
 * @return the number of cleared tiles or an error.
 */
template <class T>
CommandCost RemoveFromRailBaseStation(TileArea ta, SmallVector<T *, 4> &affected_stations, DoCommandFlag flags, Money removal_cost, bool keep_rail)
{
	/* Count of the number of tiles removed */
	int quantity = 0;
	CommandCost total_cost(EXPENSES_CONSTRUCTION);
	/* Accumulator for the errors seen during clearing. If no errors happen,
	 * and the quantity is 0 there is no station. Otherwise it will be one
	 * of the other error that got accumulated. */
	CommandCost error;

	/* Do the action for every tile into the area */
	TILE_AREA_LOOP(tile, ta) {
		/* Make sure the specified tile is a rail station */
		if (!HasStationTileRail(tile)) continue;

		/* If there is a vehicle on ground, do not allow to remove (flood) the tile */
		CommandCost ret = EnsureNoVehicleOnGround(tile);
		error.AddCost(ret);
		if (ret.Failed()) continue;

		/* Check ownership of station */
		T *st = T::GetByTile(tile);
		if (st == NULL) continue;

		if (_current_company != OWNER_WATER) {
			CommandCost ret = CheckOwnership(st->owner);
			error.AddCost(ret);
			if (ret.Failed()) continue;
		}

		/* If we reached here, the tile is valid so increase the quantity of tiles we will remove */
		quantity++;

		if (keep_rail || IsStationTileBlocked(tile)) {
			/* Don't refund the 'steel' of the track when we keep the
			 *  rail, or when the tile didn't have any rail at all. */
			total_cost.AddCost(-_price[PR_CLEAR_RAIL]);
		}

		if (flags & DC_EXEC) {
			/* read variables before the station tile is removed */
			uint specindex = GetCustomStationSpecIndex(tile);
			Track track = GetRailStationTrack(tile);
			Owner owner = GetTileOwner(tile);
			RailType rt = GetRailType(tile);
			Train *v = NULL;

			if (HasStationReservation(tile)) {
				v = GetTrainForReservation(tile, track);
				if (v != NULL) FreeTrainReservation(v);
			}

			bool build_rail = keep_rail && !IsStationTileBlocked(tile);
			if (!build_rail && !IsStationTileBlocked(tile)) Company::Get(owner)->infrastructure.rail[rt]--;

			DoClearSquare(tile);
			DeleteNewGRFInspectWindow(GSF_STATIONS, tile);
			if (build_rail) MakeRailNormal(tile, owner, TrackToTrackBits(track), rt);
			Company::Get(owner)->infrastructure.station--;
			DirtyCompanyInfrastructureWindows(owner);

			st->AfterRemoveTile(tile);
			AddTrackToSignalBuffer(tile, track, owner);
			YapfNotifyTrackLayoutChange(tile, track);

			DeallocateSpecFromStation(st, specindex);

			affected_stations.Include(st);

			if (v != NULL) RestoreTrainReservation(v);
		}
	}

	if (quantity == 0) return error.Failed() ? error : CommandCost(STR_ERROR_THERE_IS_NO_STATION);

	for (T **stp = affected_stations.Begin(); stp != affected_stations.End(); stp++) {
		T *st = *stp;

		/* now we need to make the "spanned" area of the railway station smaller
		 * if we deleted something at the edges.
		 * we also need to adjust train_tile. */
		MakeRailStationAreaSmaller(st);
		UpdateStationSignCoord(st);

		/* if we deleted the whole station, delete the train facility. */
		if (st->train_station.tile == INVALID_TILE) {
			st->facilities &= ~FACIL_TRAIN;
			SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_TRAINS);
			st->UpdateVirtCoord();
			DeleteStationIfEmpty(st);
		}
	}

	total_cost.AddCost(quantity * removal_cost);
	return total_cost;
}

/**
 * Remove a single tile from a rail station.
 * This allows for custom-built station with holes and weird layouts
 * @param start tile of station piece to remove
 * @param flags operation to perform
 * @param p1 start_tile
 * @param p2 various bitstuffed elements
 * - p2 = bit 0 - if set keep the rail
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveFromRailStation(TileIndex start, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	TileIndex end = p1 == 0 ? start : p1;
	if (start >= MapSize() || end >= MapSize()) return CMD_ERROR;

	TileArea ta(start, end);
	SmallVector<Station *, 4> affected_stations;

	CommandCost ret = RemoveFromRailBaseStation(ta, affected_stations, flags, _price[PR_CLEAR_STATION_RAIL], HasBit(p2, 0));
	if (ret.Failed()) return ret;

	/* Do all station specific functions here. */
	for (Station **stp = affected_stations.Begin(); stp != affected_stations.End(); stp++) {
		Station *st = *stp;

		if (st->train_station.tile == INVALID_TILE) SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_TRAINS);
		st->MarkTilesDirty(false);
		st->RecomputeIndustriesNear();
	}

	/* Now apply the rail cost to the number that we deleted */
	return ret;
}

/**
 * Remove a single tile from a waypoint.
 * This allows for custom-built waypoint with holes and weird layouts
 * @param start tile of waypoint piece to remove
 * @param flags operation to perform
 * @param p1 start_tile
 * @param p2 various bitstuffed elements
 * - p2 = bit 0 - if set keep the rail
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdRemoveFromRailWaypoint(TileIndex start, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	TileIndex end = p1 == 0 ? start : p1;
	if (start >= MapSize() || end >= MapSize()) return CMD_ERROR;

	TileArea ta(start, end);
	SmallVector<Waypoint *, 4> affected_stations;

	return RemoveFromRailBaseStation(ta, affected_stations, flags, _price[PR_CLEAR_WAYPOINT_RAIL], HasBit(p2, 0));
}


/**
 * Remove a rail station/waypoint
 * @param st The station/waypoint to remove the rail part from
 * @param flags operation to perform
 * @tparam T the type of station to remove
 * @return cost or failure of operation
 */
template <class T>
CommandCost RemoveRailStation(T *st, DoCommandFlag flags)
{
	/* Current company owns the station? */
	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;
	}

	/* determine width and height of platforms */
	TileArea ta = st->train_station;

	assert(ta.w != 0 && ta.h != 0);

	CommandCost cost(EXPENSES_CONSTRUCTION);
	/* clear all areas of the station */
	TILE_AREA_LOOP(tile, ta) {
		/* only remove tiles that are actually train station tiles */
		if (!st->TileBelongsToRailStation(tile)) continue;

		CommandCost ret = EnsureNoVehicleOnGround(tile);
		if (ret.Failed()) return ret;

		cost.AddCost(_price[PR_CLEAR_STATION_RAIL]);
		if (flags & DC_EXEC) {
			/* read variables before the station tile is removed */
			Track track = GetRailStationTrack(tile);
			Owner owner = GetTileOwner(tile); // _current_company can be OWNER_WATER
			Train *v = NULL;
			if (HasStationReservation(tile)) {
				v = GetTrainForReservation(tile, track);
				if (v != NULL) FreeTrainTrackReservation(v);
			}
			if (!IsStationTileBlocked(tile)) Company::Get(owner)->infrastructure.rail[GetRailType(tile)]--;
			Company::Get(owner)->infrastructure.station--;
			DoClearSquare(tile);
			DeleteNewGRFInspectWindow(GSF_STATIONS, tile);
			AddTrackToSignalBuffer(tile, track, owner);
			YapfNotifyTrackLayoutChange(tile, track);
			if (v != NULL) TryPathReserve(v, true);
		}
	}

	if (flags & DC_EXEC) {
		st->AfterRemoveRect(st->train_station);

		st->train_station.Clear();

		st->facilities &= ~FACIL_TRAIN;

		free(st->speclist);
		st->num_specs = 0;
		st->speclist  = NULL;
		st->cached_anim_triggers = 0;

		DirtyCompanyInfrastructureWindows(st->owner);
		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_TRAINS);
		st->UpdateVirtCoord();
		DeleteStationIfEmpty(st);
	}

	return cost;
}

/**
 * Remove a rail station
 * @param tile Tile of the station.
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveRailStation(TileIndex tile, DoCommandFlag flags)
{
	/* if there is flooding, remove platforms tile by tile */
	if (_current_company == OWNER_WATER) {
		return DoCommand(tile, 0, 0, DC_EXEC, CMD_REMOVE_FROM_RAIL_STATION);
	}

	Station *st = Station::GetByTile(tile);
	CommandCost cost = RemoveRailStation(st, flags);

	if (flags & DC_EXEC) st->RecomputeIndustriesNear();

	return cost;
}

/**
 * Remove a rail waypoint
 * @param tile Tile of the waypoint.
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveRailWaypoint(TileIndex tile, DoCommandFlag flags)
{
	/* if there is flooding, remove waypoints tile by tile */
	if (_current_company == OWNER_WATER) {
		return DoCommand(tile, 0, 0, DC_EXEC, CMD_REMOVE_FROM_RAIL_WAYPOINT);
	}

	return RemoveRailStation(Waypoint::GetByTile(tile), flags);
}


/**
 * @param truck_station Determines whether a stop is #ROADSTOP_BUS or #ROADSTOP_TRUCK
 * @param st The Station to do the whole procedure for
 * @return a pointer to where to link a new RoadStop*
 */
static RoadStop **FindRoadStopSpot(bool truck_station, Station *st)
{
	RoadStop **primary_stop = (truck_station) ? &st->truck_stops : &st->bus_stops;

	if (*primary_stop == NULL) {
		/* we have no roadstop of the type yet, so write a "primary stop" */
		return primary_stop;
	} else {
		/* there are stops already, so append to the end of the list */
		RoadStop *stop = *primary_stop;
		while (stop->next != NULL) stop = stop->next;
		return &stop->next;
	}
}

static CommandCost RemoveRoadStop(TileIndex tile, DoCommandFlag flags);

/**
 * Build a bus or truck stop.
 * @param tile Northernmost tile of the stop.
 * @param flags Operation to perform.
 * @param p1 bit 0..7: Width of the road stop.
 *           bit 8..15: Length of the road stop.
 * @param p2 bit 0: 0 For bus stops, 1 for truck stops.
 *           bit 1: 0 For normal stops, 1 for drive-through.
 *           bit 2..3: The roadtypes.
 *           bit 5: Allow stations directly adjacent to other stations.
 *           bit 6..7: Entrance direction (#DiagDirection).
 *           bit 16..31: Station ID to join (NEW_STATION if build new one).
 * @param text Unused.
 * @return The cost of this operation or an error.
 */
CommandCost CmdBuildRoadStop(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	bool type = HasBit(p2, 0);
	bool is_drive_through = HasBit(p2, 1);
	RoadTypes rts = Extract<RoadTypes, 2, 2>(p2);
	StationID station_to_join = GB(p2, 16, 16);
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = INVALID_STATION;
	bool distant_join = (station_to_join != INVALID_STATION);

	uint8 width = (uint8)GB(p1, 0, 8);
	uint8 lenght = (uint8)GB(p1, 8, 8);

	/* Check if the requested road stop is too big */
	if (width > _settings_game.station.station_spread || lenght > _settings_game.station.station_spread) return_cmd_error(STR_ERROR_STATION_TOO_SPREAD_OUT);
	/* Check for incorrect width / length. */
	if (width == 0 || lenght == 0) return CMD_ERROR;
	/* Check if the first tile and the last tile are valid */
	if (!IsValidTile(tile) || TileAddWrap(tile, width - 1, lenght - 1) == INVALID_TILE) return CMD_ERROR;

	TileArea roadstop_area(tile, width, lenght);

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (!HasExactlyOneBit(rts) || !HasRoadTypesAvail(_current_company, rts)) return CMD_ERROR;

	/* Trams only have drive through stops */
	if (!is_drive_through && HasBit(rts, ROADTYPE_TRAM)) return CMD_ERROR;

	DiagDirection ddir = Extract<DiagDirection, 6, 2>(p2);

	/* Safeguard the parameters. */
	if (!IsValidDiagDirection(ddir)) return CMD_ERROR;
	/* If it is a drive-through stop, check for valid axis. */
	if (is_drive_through && !IsValidAxis((Axis)ddir)) return CMD_ERROR;

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	/* Total road stop cost. */
	CommandCost cost(EXPENSES_CONSTRUCTION, roadstop_area.w * roadstop_area.h * _price[type ? PR_BUILD_STATION_TRUCK : PR_BUILD_STATION_BUS]);
	StationID est = INVALID_STATION;
	ret = CheckFlatLandRoadStop(roadstop_area, flags, is_drive_through ? 5 << ddir : 1 << ddir, is_drive_through, type, DiagDirToAxis(ddir), &est, rts);
	if (ret.Failed()) return ret;
	cost.AddCost(ret);

	Station *st = NULL;
	ret = FindJoiningStation(est, station_to_join, HasBit(p2, 5), roadstop_area, &st, STR_ERROR_MUST_REMOVE_ROAD_STOP_FIRST);
	if (ret.Failed()) return ret;

	/* Check if this number of road stops can be allocated. */
	if (!RoadStop::CanAllocateItem(roadstop_area.w * roadstop_area.h)) return_cmd_error(type ? STR_ERROR_TOO_MANY_TRUCK_STOPS : STR_ERROR_TOO_MANY_BUS_STOPS);

	ret = BuildStationPart(&st, flags, reuse, roadstop_area, STATIONNAMING_ROAD);
	if (ret.Failed()) return ret;

	if (flags & DC_EXEC) {
		/* Check every tile in the area. */
		TILE_AREA_LOOP(cur_tile, roadstop_area) {
			RoadTypes cur_rts = (IsRoadTile(cur_tile) || IsStationTile(cur_tile)) ? GetRoadTypes(cur_tile) : ROADTYPES_NONE;
			Owner road_owner = HasBit(cur_rts, ROADTYPE_ROAD) ? GetRoadOwner(cur_tile, ROADTYPE_ROAD) : _current_company;
			Owner tram_owner = HasBit(cur_rts, ROADTYPE_TRAM) ? GetRoadOwner(cur_tile, ROADTYPE_TRAM) : _current_company;

			if (IsStationTile(cur_tile) && IsRoadStop(cur_tile)) {
				RemoveRoadStop(cur_tile, flags);
			}

			RoadStop *road_stop = new RoadStop(cur_tile);
			/* Insert into linked list of RoadStops. */
			RoadStop **currstop = FindRoadStopSpot(type, st);
			*currstop = road_stop;

			if (type) {
				st->truck_station.Add(cur_tile);
			} else {
				st->bus_station.Add(cur_tile);
			}

			/* Initialize an empty station. */
			st->AddFacility((type) ? FACIL_TRUCK_STOP : FACIL_BUS_STOP, cur_tile);

			st->rect.Add (cur_tile);

			RoadStopType rs_type = type ? ROADSTOP_TRUCK : ROADSTOP_BUS;
			if (is_drive_through) {
				/* Update company infrastructure counts. If the current tile is a normal
				 * road tile, count only the new road bits needed to get a full diagonal road. */
				RoadType rt;
				FOR_EACH_SET_ROADTYPE(rt, cur_rts | rts) {
					Company *c = Company::GetIfValid(rt == ROADTYPE_ROAD ? road_owner : tram_owner);
					if (c != NULL) {
						c->infrastructure.road[rt] += 2 - (IsRoadTile(cur_tile) && HasBit(cur_rts, rt) ? CountBits(GetRoadBits(cur_tile, rt)) : 0);
						DirtyCompanyInfrastructureWindows(c->index);
					}
				}

				MakeDriveThroughRoadStop(cur_tile, st->owner, road_owner, tram_owner, st->index, rs_type, rts | cur_rts, DiagDirToAxis(ddir));
				road_stop->MakeDriveThrough();
			} else {
				/* Non-drive-through stop never overbuild and always count as two road bits. */
				Company::Get(st->owner)->infrastructure.road[FIND_FIRST_BIT(rts)] += 2;
				MakeRoadStop(cur_tile, st->owner, st->index, rs_type, rts, ddir);
			}
			Company::Get(st->owner)->infrastructure.station++;
			DirtyCompanyInfrastructureWindows(st->owner);

			MarkTileDirtyByTile(cur_tile);
		}
	}

	if (st != NULL) {
		st->UpdateVirtCoord();
		UpdateStationAcceptance(st, false);
		st->RecomputeIndustriesNear();
		InvalidateWindowData(WC_SELECT_STATION, 0, 0);
		InvalidateWindowData(WC_STATION_LIST, st->owner, 0);
		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_ROADVEHS);
	}
	return cost;
}


/**
 * Remove a bus station/truck stop
 * @param tile TileIndex been queried
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveRoadStop(TileIndex tile, DoCommandFlag flags)
{
	Station *st = Station::GetByTile(tile);

	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;
	}

	bool is_truck = IsTruckStop(tile);

	RoadStop **primary_stop;
	RoadStop *cur_stop;
	if (is_truck) { // truck stop
		primary_stop = &st->truck_stops;
		cur_stop = RoadStop::GetByTile(tile, ROADSTOP_TRUCK);
	} else {
		primary_stop = &st->bus_stops;
		cur_stop = RoadStop::GetByTile(tile, ROADSTOP_BUS);
	}

	assert(cur_stop != NULL);

	/* don't do the check for drive-through road stops when company bankrupts */
	if (IsDriveThroughStopTile(tile) && (flags & DC_BANKRUPT)) {
		/* remove the 'going through road stop' status from all vehicles on that tile */
		VehicleTileIterator iter (tile);
		while (!iter.finished()) {
			Vehicle *v = iter.next();
			if (v->type == VEH_ROAD) {
				/* Okay... we are a road vehicle on a drive through road stop.
				 * But that road stop has just been removed, so we need to make
				 * sure we are in a valid state... however, vehicles can also
				 * turn on road stop tiles, so only clear the 'road stop' state
				 * bits and only when the state was 'in road stop', otherwise
				 * we'll end up clearing the turn around bits. */
				RoadVehicle *rv = RoadVehicle::From(v);
				if (HasBit(rv->state, RVS_IN_DT_ROAD_STOP)) rv->state &= RVSB_ROAD_STOP_TRACKDIR_MASK;
			}
		}
	} else {
		CommandCost ret = EnsureNoVehicleOnGround(tile);
		if (ret.Failed()) return ret;
	}

	if (flags & DC_EXEC) {
		if (*primary_stop == cur_stop) {
			/* removed the first stop in the list */
			*primary_stop = cur_stop->next;
			/* removed the only stop? */
			if (*primary_stop == NULL) {
				st->facilities &= (is_truck ? ~FACIL_TRUCK_STOP : ~FACIL_BUS_STOP);
			}
		} else {
			/* tell the predecessor in the list to skip this stop */
			RoadStop *pred = *primary_stop;
			while (pred->next != cur_stop) pred = pred->next;
			pred->next = cur_stop->next;
		}

		/* Update company infrastructure counts. */
		RoadType rt;
		FOR_EACH_SET_ROADTYPE(rt, GetRoadTypes(tile)) {
			Company *c = Company::GetIfValid(GetRoadOwner(tile, rt));
			if (c != NULL) {
				c->infrastructure.road[rt] -= 2;
				DirtyCompanyInfrastructureWindows(c->index);
			}
		}
		Company::Get(st->owner)->infrastructure.station--;

		if (IsDriveThroughStopTile(tile)) {
			/* Clears the tile for us */
			cur_stop->ClearDriveThrough();
		} else {
			DoClearSquare(tile);
		}

		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_ROADVEHS);
		delete cur_stop;

		/* Make sure no vehicle is going to the old roadstop */
		RoadVehicle *v;
		FOR_ALL_ROADVEHICLES(v) {
			if (v->First() == v && v->current_order.IsType(OT_GOTO_STATION) &&
					v->dest_tile == tile) {
				v->dest_tile = v->GetOrderStationLocation(st->index);
			}
		}

		st->AfterRemoveTile(tile);

		st->UpdateVirtCoord();
		st->RecomputeIndustriesNear();
		DeleteStationIfEmpty(st);

		/* Update the tile area of the truck/bus stop */
		if (is_truck) {
			st->truck_station.Clear();
			for (const RoadStop *rs = st->truck_stops; rs != NULL; rs = rs->next) st->truck_station.Add(rs->xy);
		} else {
			st->bus_station.Clear();
			for (const RoadStop *rs = st->bus_stops; rs != NULL; rs = rs->next) st->bus_station.Add(rs->xy);
		}
	}

	return CommandCost(EXPENSES_CONSTRUCTION, _price[is_truck ? PR_CLEAR_STATION_TRUCK : PR_CLEAR_STATION_BUS]);
}

/**
 * Remove bus or truck stops.
 * @param tile Northernmost tile of the removal area.
 * @param flags Operation to perform.
 * @param p1 bit 0..7: Width of the removal area.
 *           bit 8..15: Height of the removal area.
 * @param p2 bit 0: 0 For bus stops, 1 for truck stops.
 * @param text Unused.
 * @return The cost of this operation or an error.
 */
CommandCost CmdRemoveRoadStop(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	uint8 width = (uint8)GB(p1, 0, 8);
	uint8 height = (uint8)GB(p1, 8, 8);

	/* Check for incorrect width / height. */
	if (width == 0 || height == 0) return CMD_ERROR;
	/* Check if the first tile and the last tile are valid */
	if (!IsValidTile(tile) || TileAddWrap(tile, width - 1, height - 1) == INVALID_TILE) return CMD_ERROR;

	TileArea roadstop_area(tile, width, height);

	int quantity = 0;
	CommandCost cost(EXPENSES_CONSTRUCTION);
	TILE_AREA_LOOP(cur_tile, roadstop_area) {
		/* Make sure the specified tile is a road stop of the correct type */
		if (!IsStationTile(cur_tile) || !IsRoadStop(cur_tile) || (uint32)GetRoadStopType(cur_tile) != GB(p2, 0, 1)) continue;

		/* Save the stop info before it is removed */
		bool is_drive_through = IsDriveThroughStopTile(cur_tile);
		RoadTypes rts = GetRoadTypes(cur_tile);
		RoadBits road_bits = IsDriveThroughStopTile(cur_tile) ?
				AxisToRoadBits(GetRoadStopAxis(cur_tile)) :
				DiagDirToRoadBits(GetRoadStopDir(cur_tile));

		Owner road_owner = GetRoadOwner(cur_tile, ROADTYPE_ROAD);
		Owner tram_owner = GetRoadOwner(cur_tile, ROADTYPE_TRAM);
		CommandCost ret = RemoveRoadStop(cur_tile, flags);
		if (ret.Failed()) return ret;
		cost.AddCost(ret);

		quantity++;
		/* If the stop was a drive-through stop replace the road */
		if ((flags & DC_EXEC) && is_drive_through) {
			MakeRoadNormal(cur_tile, road_bits, rts, ClosestTownFromTile(cur_tile)->index,
					road_owner, tram_owner);

			/* Update company infrastructure counts. */
			RoadType rt;
			FOR_EACH_SET_ROADTYPE(rt, rts) {
				Company *c = Company::GetIfValid(GetRoadOwner(cur_tile, rt));
				if (c != NULL) {
					c->infrastructure.road[rt] += CountBits(road_bits);
					DirtyCompanyInfrastructureWindows(c->index);
				}
			}
		}
	}

	if (quantity == 0) return_cmd_error(STR_ERROR_THERE_IS_NO_STATION);

	return cost;
}

/**
 * Computes the minimal distance from town's xy to any airport's tile.
 * @param it An iterator over all airport tiles.
 * @param town_tile town's tile (t->xy)
 * @return minimal manhattan distance from town_tile to any airport's tile
 */
static uint GetMinimalAirportDistanceToTile(TileIterator &it, TileIndex town_tile)
{
	uint mindist = UINT_MAX;

	for (TileIndex cur_tile = it; cur_tile != INVALID_TILE; cur_tile = ++it) {
		mindist = min(mindist, DistanceManhattan(town_tile, cur_tile));
	}

	return mindist;
}

/**
 * Get a possible noise reduction factor based on distance from town center.
 * The further you get, the less noise you generate.
 * So all those folks at city council can now happily slee...  work in their offices
 * @param as airport information
 * @param it An iterator over all airport tiles.
 * @param town_tile TileIndex of town's center, the one who will receive the airport's candidature
 * @return the noise that will be generated, according to distance
 */
uint8 GetAirportNoiseLevelForTown(const AirportSpec *as, TileIterator &it, TileIndex town_tile)
{
	/* 0 cannot be accounted, and 1 is the lowest that can be reduced from town.
	 * So no need to go any further*/
	if (as->noise_level < 2) return as->noise_level;

	uint distance = GetMinimalAirportDistanceToTile(it, town_tile);

	/* The steps for measuring noise reduction are based on the "magical" (and arbitrary) 8 base distance
	 * adding the town_council_tolerance 4 times, as a way to graduate, depending of the tolerance.
	 * Basically, it says that the less tolerant a town is, the bigger the distance before
	 * an actual decrease can be granted */
	uint8 town_tolerance_distance = 8 + (_settings_game.difficulty.town_council_tolerance * 4);

	/* now, we want to have the distance segmented using the distance judged bareable by town
	 * This will give us the coefficient of reduction the distance provides. */
	uint noise_reduction = distance / town_tolerance_distance;

	/* If the noise reduction equals the airport noise itself, don't give it for free.
	 * Otherwise, simply reduce the airport's level. */
	return noise_reduction >= as->noise_level ? 1 : as->noise_level - noise_reduction;
}

/**
 * Finds the town nearest to given airport. Based on minimal manhattan distance to any airport's tile.
 * If two towns have the same distance, town with lower index is returned.
 * @param as airport's description
 * @param it An iterator over all airport tiles
 * @return nearest town to airport
 */
Town *AirportGetNearestTown(const AirportSpec *as, const TileIterator &it)
{
	Town *t, *nearest = NULL;
	uint add = as->size_x + as->size_y - 2; // GetMinimalAirportDistanceToTile can differ from DistanceManhattan by this much
	uint mindist = UINT_MAX - add; // prevent overflow
	FOR_ALL_TOWNS(t) {
		if (DistanceManhattan(t->xy, it) < mindist + add) { // avoid calling GetMinimalAirportDistanceToTile too often
			TileIterator *copy = it.Clone();
			uint dist = GetMinimalAirportDistanceToTile(*copy, t->xy);
			delete copy;
			if (dist < mindist) {
				nearest = t;
				mindist = dist;
			}
		}
	}

	return nearest;
}


/** Recalculate the noise generated by the airports of each town */
void UpdateAirportsNoise()
{
	Town *t;
	const Station *st;

	FOR_ALL_TOWNS(t) t->noise_reached = 0;

	FOR_ALL_STATIONS(st) {
		if (st->airport.tile != INVALID_TILE && st->airport.type != AT_OILRIG) {
			const AirportSpec *as = st->airport.GetSpec();
			AirportTileIterator it(st);
			Town *nearest = AirportGetNearestTown(as, it);
			nearest->noise_reached += GetAirportNoiseLevelForTown(as, it, nearest->xy);
		}
	}
}


/**
 * Checks if an airport can be removed (no aircraft on it or landing)
 * @param st Station whose airport is to be removed
 * @param flags Operation to perform
 * @return Cost or failure of operation
 */
static CommandCost CanRemoveAirport(Station *st, DoCommandFlag flags)
{
	const Aircraft *a;
	FOR_ALL_AIRCRAFT(a) {
		if (!a->IsNormalAircraft()) continue;
		if (a->targetairport == st->index && a->state != FLYING)
			return_cmd_error(STR_ERROR_AIRCRAFT_IN_THE_WAY);
	}

	CommandCost cost(EXPENSES_CONSTRUCTION);

	TILE_AREA_LOOP(tile_cur, st->airport) {
		if (!st->TileBelongsToAirport(tile_cur)) continue;

		CommandCost ret = EnsureNoVehicleOnGround(tile_cur);
		if (ret.Failed()) return ret;

		cost.AddCost(_price[PR_CLEAR_STATION_AIRPORT]);
	}

	return cost;
}


/**
 * Place an Airport.
 * @param tile tile where airport will be built
 * @param flags operation to perform
 * @param p1
 * - p1 = (bit  0- 7) - airport type, @see airport.h
 * - p1 = (bit  8-15) - airport layout
 * @param p2 various bitstuffed elements
 * - p2 = (bit     0) - allow airports directly adjacent to other airports.
 * - p2 = (bit 16-31) - station ID to join (NEW_STATION if build new one)
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildAirport(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	StationID station_to_join = GB(p2, 16, 16);
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = INVALID_STATION;
	bool distant_join = (station_to_join != INVALID_STATION);
	byte airport_type = GB(p1, 0, 8);
	byte layout = GB(p1, 8, 8);

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	if (airport_type >= NUM_AIRPORTS) return CMD_ERROR;

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	/* Check if a valid, buildable airport was chosen for construction */
	const AirportSpec *as = AirportSpec::Get(airport_type);
	if (!as->IsAvailable() || layout >= as->num_table) return CMD_ERROR;

	Direction rotation = as->rotation[layout];
	int w = as->size_x;
	int h = as->size_y;
	if (rotation == DIR_E || rotation == DIR_W) Swap(w, h);
	TileArea airport_area = TileArea(tile, w, h);

	if (w > _settings_game.station.station_spread || h > _settings_game.station.station_spread) {
		return_cmd_error(STR_ERROR_STATION_TOO_SPREAD_OUT);
	}

	StationID est = INVALID_STATION;
	CommandCost cost = CheckFlatLandAirport(airport_area, flags, &est);
	if (cost.Failed()) return cost;

	Station *st = NULL;
	ret = FindJoiningStation(est, station_to_join, HasBit(p2, 0), airport_area, &st, STR_ERROR_MUST_DEMOLISH_AIRPORT_FIRST);
	if (ret.Failed()) return ret;

	/* Distant join */
	if (st == NULL && distant_join) st = Station::GetIfValid(station_to_join);

	ret = BuildStationPart(&st, flags, reuse, airport_area, (GetAirport(airport_type)->flags & AirportFTAClass::AIRPLANES) ? STATIONNAMING_AIRPORT : STATIONNAMING_HELIPORT);
	if (ret.Failed()) return ret;

	/* action to be performed */
	enum {
		AIRPORT_NEW,      // airport is a new station
		AIRPORT_ADD,      // add an airport to an existing station
		AIRPORT_UPGRADE,  // upgrade the airport in a station
	} action =
		(est != INVALID_STATION) ? AIRPORT_UPGRADE :
		(st != NULL) ? AIRPORT_ADD : AIRPORT_NEW;

	if (action == AIRPORT_ADD && st->airport.tile != INVALID_TILE) {
		return_cmd_error(STR_ERROR_TOO_CLOSE_TO_ANOTHER_AIRPORT);
	}

	/* The noise level is the noise from the airport and reduce it to account for the distance to the town center. */
	AirportTileTableIterator iter(as->table[layout], tile);
	Town *nearest = AirportGetNearestTown(as, iter);
	uint newnoise_level = nearest->noise_reached + GetAirportNoiseLevelForTown(as, iter, nearest->xy);

	if (action == AIRPORT_UPGRADE) {
		const AirportSpec *old_as = st->airport.GetSpec();
		AirportTileTableIterator old_iter(old_as->table[st->airport.layout], st->airport.tile);
		Town *old_nearest = AirportGetNearestTown(old_as, old_iter);
		if (old_nearest == nearest) {
			newnoise_level -= GetAirportNoiseLevelForTown(old_as, old_iter, nearest->xy);
		}
	}

	/* Check if local auth would allow a new airport */
	StringID authority_refuse_message = STR_NULL;
	Town *authority_refuse_town = NULL;

	if (_settings_game.economy.station_noise_level) {
		/* do not allow to build a new airport if this raise the town noise over the maximum allowed by town */
		if (newnoise_level > nearest->MaxTownNoise()) {
			authority_refuse_message = STR_ERROR_LOCAL_AUTHORITY_REFUSES_NOISE;
			authority_refuse_town = nearest;
		}
	} else if (action != AIRPORT_UPGRADE) {
		Town *t = ClosestTownFromTile(tile);
		uint num = 0;
		const Station *st;
		FOR_ALL_STATIONS(st) {
			if (st->town == t && (st->facilities & FACIL_AIRPORT) && st->airport.type != AT_OILRIG) num++;
		}
		if (num >= 2) {
			authority_refuse_message = STR_ERROR_LOCAL_AUTHORITY_REFUSES_AIRPORT;
			authority_refuse_town = t;
		}
	}

	if (authority_refuse_message != STR_NULL) {
		SetDParam(0, authority_refuse_town->index);
		return_cmd_error(authority_refuse_message);
	}

	if (action == AIRPORT_UPGRADE) {
		/* check that the old airport can be removed */
		CommandCost r = CanRemoveAirport(st, flags);
		if (r.Failed()) return r;
		cost.AddCost(r);
	}

	for (AirportTileTableIterator iter(as->table[layout], tile); iter != INVALID_TILE; ++iter) {
		cost.AddCost(_price[PR_BUILD_STATION_AIRPORT]);
	}

	if (flags & DC_EXEC) {
		if (action == AIRPORT_UPGRADE) {
			/* delete old airport if upgrading */
			const AirportSpec *old_as = st->airport.GetSpec();
			AirportTileTableIterator old_iter(old_as->table[st->airport.layout], st->airport.tile);
			Town *old_nearest = AirportGetNearestTown(old_as, old_iter);

			if (old_nearest != nearest) {
				old_nearest->noise_reached -= GetAirportNoiseLevelForTown(old_as, old_iter, old_nearest->xy);
				if (_settings_game.economy.station_noise_level) {
					SetWindowDirty(WC_TOWN_VIEW, st->town->index);
				}
			}

			TILE_AREA_LOOP(tile_cur, st->airport) {
				if (IsHangarTile(tile_cur)) OrderBackup::Reset(tile_cur, false);
				DeleteAnimatedTile(tile_cur);
				DoClearSquare(tile_cur);
				DeleteNewGRFInspectWindow(GSF_AIRPORTTILES, tile_cur);
			}

			for (uint i = 0; i < st->airport.GetNumHangars(); ++i) {
				DeleteWindowById(
					WC_VEHICLE_DEPOT, st->airport.GetHangarTile(i)
				);
			}

			st->AfterRemoveRect(st->airport);
			st->airport.Clear();
		}

		/* Always add the noise, so there will be no need to recalculate when option toggles */
		nearest->noise_reached = newnoise_level;

		st->AddFacility(FACIL_AIRPORT, tile);
		st->airport.type = airport_type;
		st->airport.layout = layout;
		st->airport.flags = 0;
		st->airport.rotation = rotation;

		st->rect.Add (TileArea (tile, w, h));

		for (AirportTileTableIterator iter(as->table[layout], tile); iter != INVALID_TILE; ++iter) {
			MakeAirport(iter, st->owner, st->index, iter.GetStationGfx(), WATER_CLASS_INVALID);
			SetStationTileRandomBits(iter, GB(Random(), 0, 4));
			st->airport.Add(iter);

			if (AirportTileSpec::Get(GetTranslatedAirportTileID(iter.GetStationGfx()))->animation.status != ANIM_STATUS_NO_ANIMATION) AddAnimatedTile(iter);
		}

		/* Only call the animation trigger after all tiles have been built */
		for (AirportTileTableIterator iter(as->table[layout], tile); iter != INVALID_TILE; ++iter) {
			AirportTileAnimationTrigger(st, iter, AAT_BUILT);
		}

		if (action != AIRPORT_NEW) UpdateAirplanesOnNewStation(st);

		if (action == AIRPORT_UPGRADE) {
			UpdateStationSignCoord(st);
		} else {
			Company::Get(st->owner)->infrastructure.airport++;
			DirtyCompanyInfrastructureWindows(st->owner);
			st->UpdateVirtCoord();
		}

		UpdateStationAcceptance(st, false);
		st->RecomputeIndustriesNear();
		InvalidateWindowData(WC_SELECT_STATION, 0, 0);
		InvalidateWindowData(WC_STATION_LIST, st->owner, 0);
		InvalidateWindowData(WC_STATION_VIEW, st->index, -1);

		if (_settings_game.economy.station_noise_level) {
			SetWindowDirty(WC_TOWN_VIEW, st->town->index);
		}
	}

	return cost;
}

/**
 * Remove an airport
 * @param tile TileIndex been queried
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveAirport(TileIndex tile, DoCommandFlag flags)
{
	Station *st = Station::GetByTile(tile);

	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckOwnership(st->owner);
		if (ret.Failed()) return ret;
	}

	CommandCost cost = CanRemoveAirport(st, flags);
	if (cost.Failed()) return cost;

	if (flags & DC_EXEC) {
		const AirportSpec *as = st->airport.GetSpec();
		/* The noise level is the noise from the airport and reduce it to account for the distance to the town center.
		 * And as for construction, always remove it, even if the setting is not set, in order to avoid the
		 * need of recalculation */
		AirportTileIterator it(st);
		Town *nearest = AirportGetNearestTown(as, it);
		nearest->noise_reached -= GetAirportNoiseLevelForTown(as, it, nearest->xy);

		TILE_AREA_LOOP(tile_cur, st->airport) {
			if (IsHangarTile(tile_cur)) OrderBackup::Reset(tile_cur, false);
			DeleteAnimatedTile(tile_cur);
			DoClearSquare(tile_cur);
			DeleteNewGRFInspectWindow(GSF_AIRPORTTILES, tile_cur);
		}

		/* Clear the persistent storage. */
		delete st->airport.psa;

		for (uint i = 0; i < st->airport.GetNumHangars(); ++i) {
			DeleteWindowById(
				WC_VEHICLE_DEPOT, st->airport.GetHangarTile(i)
			);
		}

		st->AfterRemoveRect(st->airport);

		st->airport.Clear();
		st->facilities &= ~FACIL_AIRPORT;

		InvalidateWindowData(WC_STATION_VIEW, st->index, -1);

		if (_settings_game.economy.station_noise_level) {
			SetWindowDirty(WC_TOWN_VIEW, st->town->index);
		}

		Company::Get(st->owner)->infrastructure.airport--;
		DirtyCompanyInfrastructureWindows(st->owner);

		st->UpdateVirtCoord();
		st->RecomputeIndustriesNear();
		DeleteStationIfEmpty(st);
		DeleteNewGRFInspectWindow(GSF_AIRPORTS, st->index);
	}

	return cost;
}

/**
 * Open/close an airport to incoming aircraft.
 * @param tile Unused.
 * @param flags Operation to perform.
 * @param p1 Station ID of the airport.
 * @param p2 Unused.
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdOpenCloseAirport(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	if (!Station::IsValidID(p1)) return CMD_ERROR;
	Station *st = Station::Get(p1);

	if (!(st->facilities & FACIL_AIRPORT) || st->owner == OWNER_NONE) return CMD_ERROR;

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	if (flags & DC_EXEC) {
		st->airport.flags ^= AIRPORT_CLOSED_block;
		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_CLOSE_AIRPORT);
	}
	return CommandCost();
}

/**
 * Tests whether the company's vehicles have this station in orders
 * @param station station ID
 * @param include_company If true only check vehicles of \a company, if false only check vehicles of other companies
 * @param company company ID
 */
bool HasStationInUse(StationID station, bool include_company, CompanyID company)
{
	const Vehicle *v;
	FOR_ALL_VEHICLES(v) {
		if ((v->owner == company) == include_company) {
			const Order *order;
			FOR_VEHICLE_ORDERS(v, order) {
				if ((order->IsType(OT_GOTO_STATION) || order->IsType(OT_GOTO_WAYPOINT)) && order->GetDestination() == station) {
					return true;
				}
			}
		}
	}
	return false;
}

/** Information about dock tile area for a given direction. */
struct DockTileArea {
	CoordDiff offset; ///< offset to northern tile
	byte width;       ///< width of dock area
	byte height;      ///< height of dock area
};

/**
 * Build a dock/haven.
 * @param tile tile where dock will be built
 * @param flags operation to perform
 * @param p1 (bit 0) - allow docks directly adjacent to other docks.
 * @param p2 bit 16-31: station ID to join (NEW_STATION if build new one)
 * @param text unused
 * @return the cost of this operation or an error
 */
CommandCost CmdBuildDock(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	static const DockTileArea dock_tilearea[DIAGDIR_END] = {
		{ { -1,  0 }, 2, 1 },
		{ {  0,  0 }, 1, 2 },
		{ {  0,  0 }, 2, 1 },
		{ {  0, -1 }, 1, 2 },
	};

	StationID station_to_join = GB(p2, 16, 16);
	bool reuse = (station_to_join != NEW_STATION);
	if (!reuse) station_to_join = INVALID_STATION;
	bool distant_join = (station_to_join != INVALID_STATION);

	if (distant_join && (!_settings_game.station.distant_join_stations || !Station::IsValidID(station_to_join))) return CMD_ERROR;

	DiagDirection direction = GetInclinedSlopeDirection(GetTileSlope(tile));
	if (direction == INVALID_DIAGDIR) return_cmd_error(STR_ERROR_SITE_UNSUITABLE);
	direction = ReverseDiagDir(direction);

	/* Docks cannot be placed on rapids */
	if (HasTileWaterGround(tile)) return_cmd_error(STR_ERROR_SITE_UNSUITABLE);

	CommandCost ret = CheckIfAuthorityAllowsNewStation(tile, flags);
	if (ret.Failed()) return ret;

	if (HasBridgeAbove(tile)) return_cmd_error(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);

	ret = DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
	if (ret.Failed()) return ret;

	TileIndex tile_cur = tile + TileOffsByDiagDir(direction);

	if (!IsWaterTile(tile_cur) || !IsTileFlat(tile_cur)) {
		return_cmd_error(STR_ERROR_SITE_UNSUITABLE);
	}

	if (HasBridgeAbove(tile_cur)) return_cmd_error(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);

	/* Get the water class of the water tile before it is cleared.*/
	WaterClass wc = GetWaterClass(tile_cur);

	ret = DoCommand(tile_cur, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
	if (ret.Failed()) return ret;

	tile_cur += TileOffsByDiagDir(direction);
	if (!IsWaterTile(tile_cur) || !IsTileFlat(tile_cur)) {
		return_cmd_error(STR_ERROR_SITE_UNSUITABLE);
	}

	TileArea dock_area = TileArea(tile + ToTileIndexDiff(dock_tilearea[direction].offset),
			dock_tilearea[direction].width, dock_tilearea[direction].height);

	/* middle */
	Station *st = NULL;
	ret = FindJoiningStation(INVALID_STATION, station_to_join, HasBit(p1, 0), dock_area, &st);
	if (ret.Failed()) return ret;

	/* Distant join */
	if (st == NULL && distant_join) st = Station::GetIfValid(station_to_join);

	/* Check if we can allocate a new dock. */
	if (!Dock::CanAllocateItem()) return_cmd_error(STR_ERROR_TOO_MANY_DOCKS);

	ret = BuildStationPart(&st, flags, reuse, dock_area, STATIONNAMING_DOCK);
	if (ret.Failed()) return ret;

	if (flags & DC_EXEC) {
		Dock **dl = &st->docks;
		while (*dl != NULL) dl = &(*dl)->next;

		*dl = new Dock(tile);
		st->dock_area.Add(dock_area);

		st->AddFacility(FACIL_DOCK, tile);

		st->rect.Add (dock_area);

		/* If the water part of the dock is on a canal, update infrastructure counts.
		 * This is needed as we've unconditionally cleared that tile before. */
		if (wc == WATER_CLASS_CANAL) {
			Company::Get(st->owner)->infrastructure.water++;
		}
		Company::Get(st->owner)->infrastructure.station += 2;
		DirtyCompanyInfrastructureWindows(st->owner);

		MakeDock(tile, st->owner, st->index, direction, wc);

		st->UpdateVirtCoord();
		UpdateStationAcceptance(st, false);
		st->RecomputeIndustriesNear();
		InvalidateWindowData(WC_SELECT_STATION, 0, 0);
		InvalidateWindowData(WC_STATION_LIST, st->owner, 0);
		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_SHIPS);
	}

	return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_STATION_DOCK]);
}

/**
 * Remove a dock
 * @param tile TileIndex been queried
 * @param flags operation to perform
 * @return cost or failure of operation
 */
static CommandCost RemoveDock(TileIndex tile, DoCommandFlag flags)
{
	assert(IsDock(tile));

	Station *st = Station::GetByTile(tile);
	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	Dock **d = &st->docks;
	TileIndex tile1, tile2;
	while ( tile1 = (*d)->xy, tile2 = tile1 + TileOffsByDiagDir(GetDockDirection(tile1)),
			tile != tile1 && tile != tile2 ) {
		/* the dock should really be there, so no check for NULL */
		d = &(*d)->next;
	}

	ret = EnsureNoVehicleOnGround(tile1);
	if (ret.Succeeded()) ret = EnsureNoVehicleOnGround(tile2);
	if (ret.Failed()) return ret;

	if (flags & DC_EXEC) {
		TileIndex docking_location = GetDockingTile(tile1);

		DoClearSquare(tile1);
		MarkTileDirtyByTile(tile1);
		MakeWaterKeepingClass(tile2, st->owner);

		st->AfterRemoveTile(tile1);
		st->AfterRemoveTile(tile2);

		Dock *next = (*d)->next;
		delete *d;
		*d = next;
		if (next == NULL && d == &st->docks) st->facilities &= ~FACIL_DOCK;

		Company::Get(st->owner)->infrastructure.station -= 2;
		DirtyCompanyInfrastructureWindows(st->owner);

		/* Update the tile area of the docks */
		st->dock_area.Clear();
		for (const Dock *dock = st->docks; dock != NULL; dock = dock->next) {
			st->dock_area.Add(dock->xy);
			st->dock_area.Add(dock->xy + TileOffsByDiagDir(GetDockDirection(dock->xy)));
		}

		SetWindowWidgetDirty(WC_STATION_VIEW, st->index, WID_SV_SHIPS);
		st->UpdateVirtCoord();
		st->RecomputeIndustriesNear();
		DeleteStationIfEmpty(st);

		/* All ships that were going to our station, can't go to it anymore.
		 * Just clear the order, then automatically the next appropriate order
		 * will be selected and in case of no appropriate order it will just
		 * wander around the world. */
		Ship *s;
		FOR_ALL_SHIPS(s) {
			if (s->current_order.IsType(OT_LOADING) && s->tile == docking_location) {
				s->LeaveStation();
			}

			if (s->dest_tile == docking_location) {
				s->dest_tile = 0;
				s->current_order.Free();
			}
		}
	}

	return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_CLEAR_STATION_DOCK]);
}

#include "table/station_land.h"

const DrawTileSprites *GetStationTileLayout(StationType st, byte gfx)
{
	return &_station_display_datas[st][gfx];
}

/**
 * Check whether a sprite is a track sprite, which can be replaced by a non-track ground sprite and a rail overlay.
 * If the ground sprite is suitable, \a ground is replaced with the new non-track ground sprite, and \a overlay_offset
 * is set to the overlay to draw.
 * @param          ti             Positional info for the tile to decide snowyness etc. May be NULL.
 * @param [in,out] ground         Groundsprite to draw.
 * @param [out]    overlay_offset Overlay to draw.
 * @return true if overlay can be drawn.
 */
bool SplitGroundSpriteForOverlay(const TileInfo *ti, SpriteID *ground, RailTrackOffset *overlay_offset)
{
	bool snow_desert;
	switch (*ground) {
		case SPR_RAIL_TRACK_X:
			snow_desert = false;
			*overlay_offset = RTO_X;
			break;

		case SPR_RAIL_TRACK_Y:
			snow_desert = false;
			*overlay_offset = RTO_Y;
			break;

		case SPR_RAIL_TRACK_X_SNOW:
			snow_desert = true;
			*overlay_offset = RTO_X;
			break;

		case SPR_RAIL_TRACK_Y_SNOW:
			snow_desert = true;
			*overlay_offset = RTO_Y;
			break;

		default:
			return false;
	}

	if (ti != NULL) {
		/* Decide snow/desert from tile */
		switch (_settings_game.game_creation.landscape) {
			case LT_ARCTIC:
				snow_desert = (uint)ti->z > GetSnowLine() * TILE_HEIGHT;
				break;

			case LT_TROPIC:
				snow_desert = GetTropicZone(ti->tile) == TROPICZONE_DESERT;
				break;

			default:
				break;
		}
	}

	*ground = snow_desert ? SPR_FLAT_SNOW_DESERT_TILE : SPR_FLAT_GRASS_TILE;
	return true;
}

static void DrawTile_Station(TileInfo *ti)
{
	const NewGRFSpriteLayout *layout = NULL;
	DrawTileSprites tmp_rail_layout;
	const DrawTileSprites *t = NULL;
	RoadTypes roadtypes;
	int32 total_offset;
	const RailtypeInfo *rti = NULL;
	uint32 relocation = 0;
	uint32 ground_relocation = 0;
	BaseStation *st = NULL;
	const StationSpec *statspec = NULL;
	uint tile_layout = 0;

	if (HasStationRail(ti->tile)) {
		rti = GetRailTypeInfo(GetRailType(ti->tile));
		roadtypes = ROADTYPES_NONE;
		total_offset = rti->GetRailtypeSpriteOffset();

		if (IsCustomStationSpecIndex(ti->tile)) {
			/* look for customization */
			st = BaseStation::GetByTile(ti->tile);
			statspec = st->speclist[GetCustomStationSpecIndex(ti->tile)].spec;

			if (statspec != NULL) {
				tile_layout = GetStationGfx(ti->tile);

				if (HasBit(statspec->callback_mask, CBM_STATION_SPRITE_LAYOUT)) {
					uint16 callback = GetStationCallback(CBID_STATION_SPRITE_LAYOUT, 0, 0, statspec, st, ti->tile);
					if (callback != CALLBACK_FAILED) tile_layout = (callback & ~1) + GetRailStationAxis(ti->tile);
				}

				/* Ensure the chosen tile layout is valid for this custom station */
				if (statspec->renderdata != NULL) {
					layout = &statspec->renderdata[tile_layout < statspec->tiles ? tile_layout : (uint)GetRailStationAxis(ti->tile)];
					if (!layout->NeedsPreprocessing()) {
						t = layout;
						layout = NULL;
					}
				}
			}
		}
	} else {
		roadtypes = IsRoadStop(ti->tile) ? GetRoadTypes(ti->tile) : ROADTYPES_NONE;
		total_offset = 0;
	}

	StationGfx gfx = GetStationGfx(ti->tile);
	if (IsAirport(ti->tile)) {
		gfx = GetAirportGfx(ti->tile);
		if (gfx >= NEW_AIRPORTTILE_OFFSET) {
			const AirportTileSpec *ats = AirportTileSpec::Get(gfx);
			if (ats->grf_prop.spritegroup[0] != NULL && DrawNewAirportTile(ti, Station::GetByTile(ti->tile), gfx, ats)) {
				return;
			}
			/* No sprite group (or no valid one) found, meaning no graphics associated.
			 * Use the substitute one instead */
			assert(ats->grf_prop.subst_id != INVALID_AIRPORTTILE);
			gfx = ats->grf_prop.subst_id;
		}
		switch (gfx) {
			case APT_RADAR_GRASS_FENCE_SW:
				t = &_station_display_datas_airport_radar_grass_fence_sw[GetAnimationFrame(ti->tile)];
				break;
			case APT_GRASS_FENCE_NE_FLAG:
				t = &_station_display_datas_airport_flag_grass_fence_ne[GetAnimationFrame(ti->tile)];
				break;
			case APT_RADAR_FENCE_SW:
				t = &_station_display_datas_airport_radar_fence_sw[GetAnimationFrame(ti->tile)];
				break;
			case APT_RADAR_FENCE_NE:
				t = &_station_display_datas_airport_radar_fence_ne[GetAnimationFrame(ti->tile)];
				break;
			case APT_GRASS_FENCE_NE_FLAG_2:
				t = &_station_display_datas_airport_flag_grass_fence_ne_2[GetAnimationFrame(ti->tile)];
				break;
		}
	}

	Owner owner = GetTileOwner(ti->tile);

	PaletteID palette;
	if (Company::IsValidID(owner)) {
		palette = COMPANY_SPRITE_COLOUR(owner);
	} else {
		/* Some stations are not owner by a company, namely oil rigs */
		palette = PALETTE_TO_GREY;
	}

	if (layout == NULL && (t == NULL || t->seq == NULL)) t = GetStationTileLayout(GetStationType(ti->tile), gfx);

	/* don't show foundation for docks */
	if (ti->tileh != SLOPE_FLAT && !IsDock(ti->tile)) {
		if (statspec != NULL && HasBit(statspec->flags, SSF_CUSTOM_FOUNDATIONS)) {
			/* Station has custom foundations.
			 * Check whether the foundation continues beyond the tile's upper sides. */
			uint edge_info = 0;
			int z;
			Slope slope = GetFoundationPixelSlope(ti->tile, &z);
			if (!HasFoundationNW(ti->tile, slope, z)) SetBit(edge_info, 0);
			if (!HasFoundationNE(ti->tile, slope, z)) SetBit(edge_info, 1);
			SpriteID image = GetCustomStationFoundationRelocation(statspec, st, ti->tile, tile_layout, edge_info);
			if (image == 0) goto draw_default_foundation;

			if (HasBit(statspec->flags, SSF_EXTENDED_FOUNDATIONS)) {
				/* Station provides extended foundations. */

				static const uint8 foundation_parts[] = {
					0, 0, 0, 0, // Invalid,  Invalid,   Invalid,   SLOPE_SW
					0, 1, 2, 3, // Invalid,  SLOPE_EW,  SLOPE_SE,  SLOPE_WSE
					0, 4, 5, 6, // Invalid,  SLOPE_NW,  SLOPE_NS,  SLOPE_NWS
					7, 8, 9     // SLOPE_NE, SLOPE_ENW, SLOPE_SEN
				};

				AddSortableSpriteToDraw(image + foundation_parts[ti->tileh], PAL_NONE, ti->x, ti->y, 16, 16, 7, ti->z);
			} else {
				/* Draw simple foundations, built up from 8 possible foundation sprites. */

				/* Each set bit represents one of the eight composite sprites to be drawn.
				 * 'Invalid' entries will not drawn but are included for completeness. */
				static const uint8 composite_foundation_parts[] = {
					/* Invalid  (00000000), Invalid   (11010001), Invalid   (11100100), SLOPE_SW  (11100000) */
					   0x00,                0xD1,                 0xE4,                 0xE0,
					/* Invalid  (11001010), SLOPE_EW  (11001001), SLOPE_SE  (11000100), SLOPE_WSE (11000000) */
					   0xCA,                0xC9,                 0xC4,                 0xC0,
					/* Invalid  (11010010), SLOPE_NW  (10010001), SLOPE_NS  (11100100), SLOPE_NWS (10100000) */
					   0xD2,                0x91,                 0xE4,                 0xA0,
					/* SLOPE_NE (01001010), SLOPE_ENW (00001001), SLOPE_SEN (01000100) */
					   0x4A,                0x09,                 0x44
				};

				uint8 parts = composite_foundation_parts[ti->tileh];

				/* If foundations continue beyond the tile's upper sides then
				 * mask out the last two pieces. */
				if (HasBit(edge_info, 0)) ClrBit(parts, 6);
				if (HasBit(edge_info, 1)) ClrBit(parts, 7);

				if (parts == 0) {
					/* We always have to draw at least one sprite to make sure there is a boundingbox and a sprite with the
					 * correct offset for the childsprites.
					 * So, draw the (completely empty) sprite of the default foundations. */
					goto draw_default_foundation;
				}

				StartSpriteCombine();
				for (int i = 0; i < 8; i++) {
					if (HasBit(parts, i)) {
						AddSortableSpriteToDraw(image + i, PAL_NONE, ti->x, ti->y, 16, 16, 7, ti->z);
					}
				}
				EndSpriteCombine();
			}

			OffsetGroundSprite(31, 1);
			ti->z += ApplyPixelFoundationToSlope(FOUNDATION_LEVELED, &ti->tileh);
		} else {
draw_default_foundation:
			DrawFoundation(ti, FOUNDATION_LEVELED);
		}
	}

	if (IsBuoy(ti->tile)) {
		DrawWaterClassGround(ti);
		SpriteID sprite = GetCanalSprite(CF_BUOY, ti->tile);
		if (sprite != 0) total_offset = sprite - SPR_IMG_BUOY;
	} else if (IsDock(ti->tile) || (IsOilRig(ti->tile) && IsTileOnWater(ti->tile))) {
		if (ti->tileh == SLOPE_FLAT) {
			DrawWaterClassGround(ti);
		} else {
			assert(IsDock(ti->tile));
			TileIndex water_tile = ti->tile + TileOffsByDiagDir(GetDockDirection(ti->tile));
			WaterClass wc = GetWaterClass(water_tile);
			if (wc == WATER_CLASS_SEA) {
				DrawShoreTile(ti->tileh);
			} else {
				DrawClearLandTile(ti, 3);
			}
		}
	} else {
		if (layout != NULL) {
			/* Sprite layout which needs preprocessing */
			bool separate_ground = HasBit(statspec->flags, SSF_SEPARATE_GROUND);
			uint32 var10_values = layout->PrepareLayout(total_offset, rti->fallback_railtype, 0, 0, separate_ground);
			uint8 var10;
			FOR_EACH_SET_BIT(var10, var10_values) {
				uint32 var10_relocation = GetCustomStationRelocation(statspec, st, ti->tile, var10);
				layout->ProcessRegisters(var10, var10_relocation, separate_ground);
			}
			tmp_rail_layout.seq = layout->GetLayout(&tmp_rail_layout.ground);
			t = &tmp_rail_layout;
			total_offset = 0;
		} else if (statspec != NULL) {
			/* Simple sprite layout */
			ground_relocation = relocation = GetCustomStationRelocation(statspec, st, ti->tile, 0);
			if (HasBit(statspec->flags, SSF_SEPARATE_GROUND)) {
				ground_relocation = GetCustomStationRelocation(statspec, st, ti->tile, 1);
			}
			ground_relocation += rti->fallback_railtype;
		}

		SpriteID image = t->ground.sprite;
		PaletteID pal  = t->ground.pal;
		RailTrackOffset overlay_offset;
		if (rti != NULL && rti->UsesOverlay() && SplitGroundSpriteForOverlay(ti, &image, &overlay_offset)) {
			SpriteID ground = GetCustomRailSprite(rti, ti->tile, RTSG_GROUND);
			DrawGroundSprite(image, PAL_NONE);
			DrawGroundSprite(ground + overlay_offset, PAL_NONE);

			if (_game_mode != GM_MENU && _settings_client.gui.show_track_reservation && HasStationReservation(ti->tile)) {
				SpriteID overlay = GetCustomRailSprite(rti, ti->tile, RTSG_OVERLAY);
				DrawGroundSprite(overlay + overlay_offset, PALETTE_CRASH);
			}
		} else {
			image += HasBit(image, SPRITE_MODIFIER_CUSTOM_SPRITE) ? ground_relocation : total_offset;
			if (HasBit(pal, SPRITE_MODIFIER_CUSTOM_SPRITE)) pal += ground_relocation;
			DrawGroundSprite(image, GroundSpritePaletteTransform(image, pal, palette));

			/* PBS debugging, draw reserved tracks darker */
			if (_game_mode != GM_MENU && _settings_client.gui.show_track_reservation && HasStationRail(ti->tile) && HasStationReservation(ti->tile)) {
				const RailtypeInfo *rti = GetRailTypeInfo(GetRailType(ti->tile));
				DrawGroundSprite(GetRailStationAxis(ti->tile) == AXIS_X ? rti->base_sprites.single_x : rti->base_sprites.single_y, PALETTE_CRASH);
			}
		}
	}

	if (HasStationRail(ti->tile) && HasCatenaryDrawn(GetRailType(ti->tile))) DrawCatenary(ti);

	if (HasBit(roadtypes, ROADTYPE_TRAM)) {
		Axis axis = GetRoadStopAxis(ti->tile); // tram stops are always drive-through
		DrawGroundSprite((HasBit(roadtypes, ROADTYPE_ROAD) ? SPR_TRAMWAY_OVERLAY : SPR_TRAMWAY_TRAM) + (axis ^ 1), PAL_NONE);
		DrawTramCatenary(ti, axis == AXIS_X ? ROAD_X : ROAD_Y);
	}

	if (IsRailWaypoint(ti->tile)) {
		/* Don't offset the waypoint graphics; they're always the same. */
		total_offset = 0;
	}

	DrawRailTileSeq(ti, t, TO_BUILDINGS, total_offset, relocation, palette);
}

void StationPickerDrawSprite(int x, int y, StationType st, RailType railtype, RoadType roadtype, int image)
{
	int32 total_offset = 0;
	PaletteID pal = COMPANY_SPRITE_COLOUR(_local_company);
	const DrawTileSprites *t = GetStationTileLayout(st, image);
	const RailtypeInfo *rti = NULL;

	if (railtype != INVALID_RAILTYPE) {
		rti = GetRailTypeInfo(railtype);
		total_offset = rti->GetRailtypeSpriteOffset();
	}

	SpriteID img = t->ground.sprite;
	RailTrackOffset overlay_offset;
	if (rti != NULL && rti->UsesOverlay() && SplitGroundSpriteForOverlay(NULL, &img, &overlay_offset)) {
		SpriteID ground = GetCustomRailSprite(rti, INVALID_TILE, RTSG_GROUND);
		DrawSprite(img, PAL_NONE, x, y);
		DrawSprite(ground + overlay_offset, PAL_NONE, x, y);
	} else {
		DrawSprite(img + total_offset, HasBit(img, PALETTE_MODIFIER_COLOUR) ? pal : PAL_NONE, x, y);
	}

	if (roadtype == ROADTYPE_TRAM) {
		DrawSprite(SPR_TRAMWAY_TRAM + (t->ground.sprite == SPR_ROAD_PAVED_STRAIGHT_X ? 1 : 0), PAL_NONE, x, y);
	}

	/* Default waypoint has no railtype specific sprites */
	DrawRailTileSeqInGUI(x, y, t, st == STATION_WAYPOINT ? 0 : total_offset, 0, pal);
}

static int GetSlopePixelZ_Station(TileIndex tile, uint x, uint y)
{
	return GetTileMaxPixelZ(tile);
}

static Foundation GetFoundation_Station(TileIndex tile, Slope tileh)
{
	return FlatteningFoundation(tileh);
}

static void GetTileDesc_Station(TileIndex tile, TileDesc *td)
{
	td->owner[0] = GetTileOwner(tile);
	if (IsDriveThroughStopTile(tile)) {
		Owner road_owner = INVALID_OWNER;
		Owner tram_owner = INVALID_OWNER;
		RoadTypes rts = GetRoadTypes(tile);
		if (HasBit(rts, ROADTYPE_ROAD)) road_owner = GetRoadOwner(tile, ROADTYPE_ROAD);
		if (HasBit(rts, ROADTYPE_TRAM)) tram_owner = GetRoadOwner(tile, ROADTYPE_TRAM);

		/* Is there a mix of owners? */
		if ((tram_owner != INVALID_OWNER && tram_owner != td->owner[0]) ||
				(road_owner != INVALID_OWNER && road_owner != td->owner[0])) {
			uint i = 1;
			if (road_owner != INVALID_OWNER) {
				td->owner_type[i] = STR_LAND_AREA_INFORMATION_ROAD_OWNER;
				td->owner[i] = road_owner;
				i++;
			}
			if (tram_owner != INVALID_OWNER) {
				td->owner_type[i] = STR_LAND_AREA_INFORMATION_TRAM_OWNER;
				td->owner[i] = tram_owner;
			}
		}
	}
	td->build_date = BaseStation::GetByTile(tile)->build_date;

	if (HasStationTileRail(tile)) {
		const StationSpec *spec = GetStationSpec(tile);

		if (spec != NULL) {
			td->station_class = StationClass::Get(spec->cls_id)->name;
			td->station_name  = spec->name;

			if (spec->grf_prop.grffile != NULL) {
				const GRFConfig *gc = GetGRFConfig(spec->grf_prop.grffile->grfid);
				td->grf = gc->GetName();
			}
		}

		const RailtypeInfo *rti = GetRailTypeInfo(GetRailType(tile));
		td->rail_speed = rti->max_speed;
	}

	if (IsAirport(tile)) {
		const AirportSpec *as = Station::GetByTile(tile)->airport.GetSpec();
		td->airport_class = AirportClass::Get(as->cls_id)->name;
		td->airport_name = as->name;

		const AirportTileSpec *ats = AirportTileSpec::GetByTile(tile);
		td->airport_tile_name = ats->name;

		if (as->grf_prop.grffile != NULL) {
			const GRFConfig *gc = GetGRFConfig(as->grf_prop.grffile->grfid);
			td->grf = gc->GetName();
		} else if (ats->grf_prop.grffile != NULL) {
			const GRFConfig *gc = GetGRFConfig(ats->grf_prop.grffile->grfid);
			td->grf = gc->GetName();
		}
	}

	StringID str;
	switch (GetStationType(tile)) {
		default: NOT_REACHED();
		case STATION_RAIL:     str = STR_LAI_STATION_DESCRIPTION_RAILROAD_STATION; break;
		case STATION_AIRPORT:
			str = (IsHangar(tile) ? STR_LAI_STATION_DESCRIPTION_AIRCRAFT_HANGAR : STR_LAI_STATION_DESCRIPTION_AIRPORT);
			break;
		case STATION_TRUCK:    str = STR_LAI_STATION_DESCRIPTION_TRUCK_LOADING_AREA; break;
		case STATION_BUS:      str = STR_LAI_STATION_DESCRIPTION_BUS_STATION; break;
		case STATION_OILRIG:   str = STR_INDUSTRY_NAME_OIL_RIG; break;
		case STATION_DOCK:     str = STR_LAI_STATION_DESCRIPTION_SHIP_DOCK; break;
		case STATION_BUOY:     str = STR_LAI_STATION_DESCRIPTION_BUOY; break;
		case STATION_WAYPOINT: str = STR_LAI_STATION_DESCRIPTION_WAYPOINT; break;
	}
	td->str = str;
}


static TrackStatus GetTileRailwayStatus_Station(TileIndex tile, DiagDirection side)
{
	if (!HasStationRail(tile) || IsStationTileBlocked(tile)) return 0;

	return CombineTrackStatus(TrackBitsToTrackdirBits(GetRailStationTrackBits(tile)), TRACKDIR_BIT_NONE);
}

static TrackStatus GetTileRoadStatus_Station(TileIndex tile, uint sub_mode, DiagDirection side)
{
	if (!IsRoadStop(tile) || (GetRoadTypes(tile) & sub_mode) == 0) return 0;

	TrackBits trackbits;

	if (IsStandardRoadStopTile(tile)) {
		DiagDirection dir = GetRoadStopDir(tile);

		if (side != INVALID_DIAGDIR && dir != side) return 0;

		trackbits = DiagDirToDiagTrackBits(dir);
	} else {
		Axis axis = GetRoadStopAxis(tile);

		if (side != INVALID_DIAGDIR && axis != DiagDirToAxis(side)) return 0;

		trackbits = AxisToTrackBits(axis);
	}

	return CombineTrackStatus(TrackBitsToTrackdirBits(trackbits), TRACKDIR_BIT_NONE);
}

static TrackdirBits GetTileWaterwayStatus_Station(TileIndex tile, DiagDirection side)
{
	if (!IsBuoy(tile)) return TRACKDIR_BIT_NONE;

	/* buoy is coded as a station, it is always on open water */
	TrackBits trackbits = TRACK_BIT_ALL;
	/* remove tracks that connect NE map edge */
	if (TileX(tile) == 0) trackbits &= ~(TRACK_BIT_X | TRACK_BIT_UPPER | TRACK_BIT_RIGHT);
	/* remove tracks that connect NW map edge */
	if (TileY(tile) == 0) trackbits &= ~(TRACK_BIT_Y | TRACK_BIT_LEFT | TRACK_BIT_UPPER);

	return TrackBitsToTrackdirBits(trackbits);
}


static void TileLoop_Station(TileIndex tile)
{
	/* FIXME -- GetTileTrackStatus_Station -> animated stationtiles
	 * hardcoded.....not good */
	switch (GetStationType(tile)) {
		case STATION_AIRPORT:
			AirportTileAnimationTrigger(Station::GetByTile(tile), tile, AAT_TILELOOP);
			break;

		case STATION_DOCK:
			if (!IsTileFlat(tile)) break; // only handle water part
			/* FALL THROUGH */
		case STATION_OILRIG: //(station part)
		case STATION_BUOY:
			TileLoop_Water(tile);
			break;

		default: break;
	}
}


static void AnimateTile_Station(TileIndex tile)
{
	if (HasStationRail(tile)) {
		AnimateStationTile(tile);
		return;
	}

	if (IsAirport(tile)) {
		AnimateAirportTile(tile);
	}
}


static bool ClickTile_Station(TileIndex tile)
{
	const BaseStation *bst = BaseStation::GetByTile(tile);

	if (bst->IsWaypoint()) {
		ShowWaypointWindow(Waypoint::From(bst));
	} else if (IsHangar(tile)) {
		const Station *st = Station::From(bst);
		ShowDepotWindow(st->airport.GetHangarTile(st->airport.GetHangarNum(tile)), VEH_AIRCRAFT);
	} else {
		ShowStationViewWindow(bst->index);
	}
	return true;
}

/**
 * Run the watched cargo callback for all houses in the catchment area.
 * @param st Station.
 */
void TriggerWatchedCargoCallbacks(Station *st)
{
	/* Collect cargoes accepted since the last big tick. */
	uint cargoes = 0;
	for (CargoID cid = 0; cid < NUM_CARGO; cid++) {
		if (HasBit(st->goods[cid].status, GoodsEntry::GES_ACCEPTED_BIGTICK)) SetBit(cargoes, cid);
	}

	/* Anything to do? */
	if (cargoes == 0) return;

	/* Loop over all houses in the catchment. */
	TileArea ta = st->GetCatchmentArea();
	TILE_AREA_LOOP(tile, ta) {
		if (IsHouseTile(tile)) {
			WatchedCargoCallback(tile, cargoes);
		}
	}
}

/**
 * This function is called for each station once every 250 ticks.
 * Not all stations will get the tick at the same time.
 * @param st the station receiving the tick.
 * @return true if the station is still valid (wasn't deleted)
 */
static bool StationHandleBigTick(BaseStation *st)
{
	if (!st->IsInUse()) {
		if (++st->delete_ctr >= 8) delete st;
		return false;
	}

	if (!st->IsWaypoint()) {
		TriggerWatchedCargoCallbacks(Station::From(st));

		for (CargoID i = 0; i < NUM_CARGO; i++) {
			ClrBit(Station::From(st)->goods[i].status, GoodsEntry::GES_ACCEPTED_BIGTICK);
		}

		UpdateStationAcceptance(Station::From(st), true);
	}

	return true;
}

static inline void byte_inc_sat(byte *p)
{
	byte b = *p + 1;
	if (b != 0) *p = b;
}

static void UpdateStationRating(Station *st)
{
	bool waiting_changed = false;

	byte_inc_sat(&st->time_since_load);
	byte_inc_sat(&st->time_since_unload);

	const CargoSpec *cs;
	FOR_ALL_CARGOSPECS(cs) {
		GoodsEntry *ge = &st->goods[cs->Index()];
		/* Slowly increase the rating back to his original level in the case we
		 *  didn't deliver cargo yet to this station. This happens when a bribe
		 *  failed while you didn't moved that cargo yet to a station. */
		if (!ge->HasRating() && ge->rating < INITIAL_STATION_RATING) {
			ge->rating++;
		}

		/* Only change the rating if we are moving this cargo */
		if (ge->HasRating()) {
			byte_inc_sat(&ge->time_since_pickup);

			bool skip = false;
			int rating = 0;
			uint waiting = ge->cargo.TotalCount();

			/* num_dests is at least 1 if there is any cargo as
			 * INVALID_STATION is also a destination.
			 */
			uint num_dests = (uint)ge->cargo.Packets()->MapSize();

			/* Average amount of cargo per next hop, but prefer solitary stations
			 * with only one or two next hops. They are allowed to have more
			 * cargo waiting per next hop.
			 * With manual cargo distribution waiting_avg = waiting / 2 as then
			 * INVALID_STATION is the only destination.
			 */
			uint waiting_avg = waiting / (num_dests + 1);

			if (HasBit(cs->callback_mask, CBM_CARGO_STATION_RATING_CALC)) {
				/* Perform custom station rating. If it succeeds the speed, days in transit and
				 * waiting cargo ratings must not be executed. */

				/* NewGRFs expect last speed to be 0xFF when no vehicle has arrived yet. */
				uint last_speed = ge->HasVehicleEverTriedLoading() ? ge->last_speed : 0xFF;

				uint32 var18 = min(ge->time_since_pickup, 0xFF) | (min(ge->max_waiting_cargo, 0xFFFF) << 8) | (min(last_speed, 0xFF) << 24);
				/* Convert to the 'old' vehicle types */
				uint32 var10 = (st->last_vehicle_type == VEH_INVALID) ? 0x0 : (st->last_vehicle_type + 0x10);
				uint16 callback = GetCargoCallback(CBID_CARGO_STATION_RATING_CALC, var10, var18, cs);
				if (callback != CALLBACK_FAILED) {
					skip = true;
					rating = GB(callback, 0, 14);

					/* Simulate a 15 bit signed value */
					if (HasBit(callback, 14)) rating -= 0x4000;
				}
			}

			if (!skip) {
				int b = ge->last_speed - 85;
				if (b >= 0) rating += b >> 2;

				byte waittime = ge->time_since_pickup;
				if (st->last_vehicle_type == VEH_SHIP) waittime >>= 2;
				(waittime > 21) ||
				(rating += 25, waittime > 12) ||
				(rating += 25, waittime > 6) ||
				(rating += 45, waittime > 3) ||
				(rating += 35, true);

				(rating -= 90, ge->max_waiting_cargo > 1500) ||
				(rating += 55, ge->max_waiting_cargo > 1000) ||
				(rating += 35, ge->max_waiting_cargo > 600) ||
				(rating += 10, ge->max_waiting_cargo > 300) ||
				(rating += 20, ge->max_waiting_cargo > 100) ||
				(rating += 10, true);
			}

			if (Company::IsValidID(st->owner) && HasBit(st->town->statues, st->owner)) rating += 26;

			byte age = ge->last_age;
			(age >= 3) ||
			(rating += 10, age >= 2) ||
			(rating += 10, age >= 1) ||
			(rating += 13, true);

			{
				int or_ = ge->rating; // old rating

				/* only modify rating in steps of -2, -1, 0, 1 or 2 */
				ge->rating = rating = or_ + Clamp(Clamp(rating, 0, 255) - or_, -2, 2);

				/* if rating is <= 64 and more than 100 items waiting on average per destination,
				 * remove some random amount of goods from the station */
				if (rating <= 64 && waiting_avg >= 100) {
					int dec = Random() & 0x1F;
					if (waiting_avg < 200) dec &= 7;
					waiting -= (dec + 1) * num_dests;
					waiting_changed = true;
				}

				/* if rating is <= 127 and there are any items waiting, maybe remove some goods. */
				if (rating <= 127 && waiting != 0) {
					uint32 r = Random();
					if (rating <= (int)GB(r, 0, 7)) {
						/* Need to have int, otherwise it will just overflow etc. */
						waiting = max((int)waiting - (int)((GB(r, 8, 2) - 1) * num_dests), 0);
						waiting_changed = true;
					}
				}

				/* At some point we really must cap the cargo. Previously this
				 * was a strict 4095, but now we'll have a less strict, but
				 * increasingly aggressive truncation of the amount of cargo. */
				static const uint WAITING_CARGO_THRESHOLD  = 1 << 12;
				static const uint WAITING_CARGO_CUT_FACTOR = 1 <<  6;
				static const uint MAX_WAITING_CARGO        = 1 << 15;

				if (waiting > WAITING_CARGO_THRESHOLD) {
					uint difference = waiting - WAITING_CARGO_THRESHOLD;
					waiting -= (difference / WAITING_CARGO_CUT_FACTOR);

					waiting = min(waiting, MAX_WAITING_CARGO);
					waiting_changed = true;
				}

				/* We can't truncate cargo that's already reserved for loading.
				 * Thus StoredCount() here. */
				if (waiting_changed && waiting < ge->cargo.AvailableCount()) {
					/* Feed back the exact own waiting cargo at this station for the
					 * next rating calculation. */
					ge->max_waiting_cargo = 0;

					/* If truncating also punish the source stations' ratings to
					 * decrease the flow of incoming cargo. */

					StationCargoAmountMap waiting_per_source;
					ge->cargo.Truncate(ge->cargo.AvailableCount() - waiting, &waiting_per_source);
					for (StationCargoAmountMap::iterator i(waiting_per_source.begin()); i != waiting_per_source.end(); ++i) {
						Station *source_station = Station::GetIfValid(i->first);
						if (source_station == NULL) continue;

						GoodsEntry &source_ge = source_station->goods[cs->Index()];
						source_ge.max_waiting_cargo = max(source_ge.max_waiting_cargo, i->second);
					}
				} else {
					/* If the average number per next hop is low, be more forgiving. */
					ge->max_waiting_cargo = waiting_avg;
				}
			}
		}
	}

	StationID index = st->index;
	if (waiting_changed) {
		SetWindowDirty(WC_STATION_VIEW, index); // update whole window
	} else {
		SetWindowWidgetDirty(WC_STATION_VIEW, index, WID_SV_ACCEPT_RATING_LIST); // update only ratings list
	}
}

/**
 * Reroute cargo of type c at station st or in any vehicles unloading there.
 * Make sure the cargo's new next hop is neither "avoid" nor "avoid2".
 * @param st Station to be rerouted at.
 * @param c Type of cargo.
 * @param avoid Original next hop of cargo, avoid this.
 * @param avoid2 Another station to be avoided when rerouting.
 */
void RerouteCargo(Station *st, CargoID c, StationID avoid, StationID avoid2)
{
	GoodsEntry &ge = st->goods[c];

	/* Reroute cargo in station. */
	ge.cargo.Reroute(UINT_MAX, &ge.cargo, avoid, avoid2, &ge);

	/* Reroute cargo staged to be transfered. */
	for (std::list<Vehicle *>::iterator it(st->loading_vehicles.begin()); it != st->loading_vehicles.end(); ++it) {
		for (Vehicle *v = *it; v != NULL; v = v->Next()) {
			if (v->cargo_type != c) continue;
			v->cargo.Reroute(UINT_MAX, &v->cargo, avoid, avoid2, &ge);
		}
	}
}

/**
 * Check all next hops of cargo packets in this station for existance of a
 * a valid link they may use to travel on. Reroute any cargo not having a valid
 * link and remove timed out links found like this from the linkgraph. We're
 * not all links here as that is expensive and useless. A link no one is using
 * doesn't hurt either.
 * @param from Station to check.
 */
void DeleteStaleLinks(Station *from)
{
	for (CargoID c = 0; c < NUM_CARGO; ++c) {
		GoodsEntry &ge = from->goods[c];
		LinkGraph *lg = LinkGraph::GetIfValid(ge.link_graph);
		if (lg == NULL) continue;
		Node node = (*lg)[ge.node];
		for (EdgeIterator it(node.Begin()); it != node.End();) {
			Edge edge = it->second;
			Station *to = Station::Get((*lg)[it->first].Station());
			assert(to->goods[c].node == it->first);
			++it; // Do that before removing the edge. Anything else may crash.
			assert(_date >= edge.LastUpdate());
			uint timeout = LinkGraph::MIN_TIMEOUT_DISTANCE + (DistanceManhattan(from->xy, to->xy) >> 3);
			if ((uint)(_date - edge.LastUpdate()) > timeout) {
				/* Have all vehicles refresh their next hops before deciding to
				 * remove the node. */
				bool updated = false;
				OrderList *l;
				FOR_ALL_ORDER_LISTS(l) {
					bool found_from = false;
					bool found_to = false;
					for (Order *order = l->GetFirstOrder(); order != NULL; order = order->next) {
						if (!order->IsType(OT_GOTO_STATION) && !order->IsType(OT_IMPLICIT)) continue;
						if (order->GetDestination() == from->index) {
							found_from = true;
							if (found_to) break;
						} else if (order->GetDestination() == to->index) {
							found_to = true;
							if (found_from) break;
						}
					}
					if (!found_to || !found_from) continue;
					for (Vehicle *v = l->GetFirstSharedVehicle(); !updated && v != NULL; v = v->NextShared()) {
						/* There is potential for optimization here:
						 * - Usually consists of the same order list are the same. It's probably better to
						 *   first check the first of each list, then the second of each list and so on.
						 * - We could try to figure out if we've seen a consist with the same cargo on the
						 *   same list already and if the consist can actually carry the cargo we're looking
						 *   for. With conditional and refit orders this is not quite trivial, though. */
						LinkRefresher::Run(v, false); // Don't allow merging. Otherwise lg might get deleted.
						if (edge.LastUpdate() == _date) updated = true;
					}
					if (updated) break;
				}
				if (!updated) {
					/* If it's still considered dead remove it. */
					node.RemoveEdge(to->goods[c].node);
					ge.flows.DeleteFlows(to->index);
					RerouteCargo(from, c, to->index, from->index);
				}
			} else if (edge.LastUnrestrictedUpdate() != INVALID_DATE && (uint)(_date - edge.LastUnrestrictedUpdate()) > timeout) {
				edge.Restrict();
				ge.flows.RestrictFlows(to->index);
				RerouteCargo(from, c, to->index, from->index);
			} else if (edge.LastRestrictedUpdate() != INVALID_DATE && (uint)(_date - edge.LastRestrictedUpdate()) > timeout) {
				edge.Release();
			}
		}
		assert(_date >= lg->LastCompression());
		if ((uint)(_date - lg->LastCompression()) > LinkGraph::COMPRESSION_INTERVAL) {
			lg->Compress();
		}
	}
}

/**
 * Increase capacity for a link stat given by station cargo and next hop.
 * @param st Station to get the link stats from.
 * @param cargo Cargo to increase stat for.
 * @param next_station_id Station the consist will be travelling to next.
 * @param capacity Capacity to add to link stat.
 * @param usage Usage to add to link stat.
 * @param mode Update mode to be applied.
 */
void IncreaseStats(Station *st, CargoID cargo, StationID next_station_id, uint capacity, uint usage, EdgeUpdateMode mode)
{
	GoodsEntry &ge1 = st->goods[cargo];
	Station *st2 = Station::Get(next_station_id);
	GoodsEntry &ge2 = st2->goods[cargo];
	LinkGraph *lg = NULL;
	if (ge1.link_graph == INVALID_LINK_GRAPH) {
		if (ge2.link_graph == INVALID_LINK_GRAPH) {
			if (LinkGraph::CanAllocateItem()) {
				lg = new LinkGraph(cargo);
				LinkGraphSchedule::Instance()->Queue(lg);
				ge2.link_graph = lg->index;
				ge2.node = lg->AddNode(st2);
			} else {
				DEBUG(misc, 0, "Can't allocate link graph");
			}
		} else {
			lg = LinkGraph::Get(ge2.link_graph);
		}
		if (lg) {
			ge1.link_graph = lg->index;
			ge1.node = lg->AddNode(st);
		}
	} else if (ge2.link_graph == INVALID_LINK_GRAPH) {
		lg = LinkGraph::Get(ge1.link_graph);
		ge2.link_graph = lg->index;
		ge2.node = lg->AddNode(st2);
	} else {
		lg = LinkGraph::Get(ge1.link_graph);
		if (ge1.link_graph != ge2.link_graph) {
			LinkGraph *lg2 = LinkGraph::Get(ge2.link_graph);
			if (lg->Size() < lg2->Size()) {
				LinkGraphSchedule::Instance()->Unqueue(lg);
				lg2->Merge(lg); // Updates GoodsEntries of lg
				lg = lg2;
			} else {
				LinkGraphSchedule::Instance()->Unqueue(lg2);
				lg->Merge(lg2); // Updates GoodsEntries of lg2
			}
		}
	}
	if (lg != NULL) {
		(*lg)[ge1.node].UpdateEdge(ge2.node, capacity, usage, mode);
	}
}

/**
 * Increase capacity for all link stats associated with vehicles in the given consist.
 * @param st Station to get the link stats from.
 * @param front First vehicle in the consist.
 * @param next_station_id Station the consist will be travelling to next.
 */
void IncreaseStats(Station *st, const Vehicle *front, StationID next_station_id)
{
	for (const Vehicle *v = front; v != NULL; v = v->Next()) {
		if (v->refit_cap > 0) {
			/* The cargo count can indeed be higher than the refit_cap if
			 * wagons have been auto-replaced and subsequently auto-
			 * refitted to a higher capacity. The cargo gets redistributed
			 * among the wagons in that case.
			 * As usage is not such an important figure anyway we just
			 * ignore the additional cargo then.*/
			IncreaseStats(st, v->cargo_type, next_station_id, v->refit_cap,
				min(v->refit_cap, v->cargo.StoredCount()), EUM_INCREASE);
		}
	}
}

/* called for every station each tick */
static void StationHandleSmallTick(BaseStation *st)
{
	if (st->IsWaypoint() || !st->IsInUse()) return;

	byte b = st->delete_ctr + 1;
	if (b >= STATION_RATING_TICKS) b = 0;
	st->delete_ctr = b;

	if (b == 0) UpdateStationRating(Station::From(st));
}

void OnTick_Station()
{
	if (_game_mode == GM_EDITOR) return;

	BaseStation *st;
	FOR_ALL_BASE_STATIONS(st) {
		StationHandleSmallTick(st);

		/* Clean up the link graph about once a week. */
		if (!st->IsWaypoint() && (_tick_counter + st->index) % STATION_LINKGRAPH_TICKS == 0) {
			DeleteStaleLinks(Station::From(st));
		};

		/* Run STATION_ACCEPTANCE_TICKS = 250 tick interval trigger for station animation.
		 * Station index is included so that triggers are not all done
		 * at the same time. */
		if ((_tick_counter + st->index) % STATION_ACCEPTANCE_TICKS == 0) {
			/* Stop processing this station if it was deleted */
			if (!StationHandleBigTick(st)) continue;
			TriggerStationAnimation(st, st->xy, SAT_250_TICKS);
			if (!st->IsWaypoint()) AirportAnimationTrigger(Station::From(st), AAT_STATION_250_TICKS);
		}
	}
}

/** Monthly loop for stations. */
void StationMonthlyLoop()
{
	Station *st;

	FOR_ALL_STATIONS(st) {
		for (CargoID i = 0; i < NUM_CARGO; i++) {
			GoodsEntry *ge = &st->goods[i];
			SB(ge->status, GoodsEntry::GES_LAST_MONTH, 1, GB(ge->status, GoodsEntry::GES_CURRENT_MONTH, 1));
			ClrBit(ge->status, GoodsEntry::GES_CURRENT_MONTH);
		}
	}
}


void ModifyStationRatingAround(TileIndex tile, Owner owner, int amount, uint radius)
{
	Station *st;

	FOR_ALL_STATIONS(st) {
		if (st->owner == owner &&
				DistanceManhattan(tile, st->xy) <= radius) {
			for (CargoID i = 0; i < NUM_CARGO; i++) {
				GoodsEntry *ge = &st->goods[i];

				if (ge->status != 0) {
					ge->rating = Clamp(ge->rating + amount, 0, 255);
				}
			}
		}
	}
}

static uint UpdateStationWaiting(Station *st, CargoID type, uint amount, SourceType source_type, SourceID source_id)
{
	/* We can't allocate a CargoPacket? Then don't do anything
	 * at all; i.e. just discard the incoming cargo. */
	if (!CargoPacket::CanAllocateItem()) return 0;

	GoodsEntry &ge = st->goods[type];
	amount += ge.amount_fract;
	ge.amount_fract = GB(amount, 0, 8);

	amount >>= 8;
	/* No new "real" cargo item yet. */
	if (amount == 0) return 0;

	StationID next = ge.GetVia(st->index);
	ge.cargo.Append(new CargoPacket(st->index, st->xy, amount, source_type, source_id), next);
	LinkGraph *lg = NULL;
	if (ge.link_graph == INVALID_LINK_GRAPH) {
		if (LinkGraph::CanAllocateItem()) {
			lg = new LinkGraph(type);
			LinkGraphSchedule::Instance()->Queue(lg);
			ge.link_graph = lg->index;
			ge.node = lg->AddNode(st);
		} else {
			DEBUG(misc, 0, "Can't allocate link graph");
		}
	} else {
		lg = LinkGraph::Get(ge.link_graph);
	}
	if (lg != NULL) (*lg)[ge.node].UpdateSupply(amount);

	if (!ge.HasRating()) {
		InvalidateWindowData(WC_STATION_LIST, st->index);
		SetBit(ge.status, GoodsEntry::GES_RATING);
	}

	TriggerStationRandomisation(st, st->xy, SRT_NEW_CARGO, type);
	TriggerStationAnimation(st, st->xy, SAT_NEW_CARGO, type);
	AirportAnimationTrigger(st, AAT_STATION_NEW_CARGO, type);

	SetWindowDirty(WC_STATION_VIEW, st->index);
	st->MarkTilesDirty(true);
	return amount;
}

static bool IsUniqueStationName(const char *name)
{
	const Station *st;

	FOR_ALL_STATIONS(st) {
		if (st->name != NULL && strcmp(st->name, name) == 0) return false;
	}

	return true;
}

/**
 * Rename a station
 * @param tile unused
 * @param flags operation to perform
 * @param p1 station ID that is to be renamed
 * @param p2 unused
 * @param text the new name or an empty string when resetting to the default
 * @return the cost of this operation or an error
 */
CommandCost CmdRenameStation(TileIndex tile, DoCommandFlag flags, uint32 p1, uint32 p2, const char *text)
{
	Station *st = Station::GetIfValid(p1);
	if (st == NULL) return CMD_ERROR;

	CommandCost ret = CheckOwnership(st->owner);
	if (ret.Failed()) return ret;

	bool reset = StrEmpty(text);

	if (!reset) {
		if (Utf8StringLength(text) >= MAX_LENGTH_STATION_NAME_CHARS) return CMD_ERROR;
		if (!IsUniqueStationName(text)) return_cmd_error(STR_ERROR_NAME_MUST_BE_UNIQUE);
	}

	if (flags & DC_EXEC) {
		free(st->name);
		st->name = reset ? NULL : xstrdup(text);

		st->UpdateVirtCoord();
		InvalidateWindowData(WC_STATION_LIST, st->owner, 1);
	}

	return CommandCost();
}

/**
 * Find all stations around a rectangular producer (industry, house, headquarter, ...)
 *
 * @param location The location/area of the producer
 * @param stations The list to store the stations in
 */
void FindStationsAroundTiles(const TileArea &location, StationList *stations)
{
	/* area to search = producer plus station catchment radius */
	uint max_rad = (_settings_game.station.modified_catchment ? MAX_CATCHMENT : CA_UNMODIFIED);

	uint x = TileX(location.tile);
	uint y = TileY(location.tile);

	uint min_x = (x > max_rad) ? x - max_rad : 0;
	uint max_x = x + location.w + max_rad;
	uint min_y = (y > max_rad) ? y - max_rad : 0;
	uint max_y = y + location.h + max_rad;

	if (min_x == 0 && _settings_game.construction.freeform_edges) min_x = 1;
	if (min_y == 0 && _settings_game.construction.freeform_edges) min_y = 1;
	if (max_x >= MapSizeX()) max_x = MapSizeX() - 1;
	if (max_y >= MapSizeY()) max_y = MapSizeY() - 1;

	for (uint cy = min_y; cy < max_y; cy++) {
		for (uint cx = min_x; cx < max_x; cx++) {
			TileIndex cur_tile = TileXY(cx, cy);
			if (!IsStationTile(cur_tile)) continue;

			Station *st = Station::GetByTile(cur_tile);
			/* st can be NULL in case of waypoints */
			if (st == NULL) continue;

			if (_settings_game.station.modified_catchment) {
				int rad = st->GetCatchmentRadius();
				int rad_x = cx - x;
				int rad_y = cy - y;

				if (rad_x < -rad || rad_x >= rad + location.w) continue;
				if (rad_y < -rad || rad_y >= rad + location.h) continue;
			}

			/* Insert the station in the set. This will fail if it has
			 * already been added.
			 */
			stations->Include(st);
		}
	}
}

/**
 * Run a tile loop to find stations around a tile, on demand. Cache the result for further requests
 * @return pointer to a StationList containing all stations found
 */
const StationList *StationFinder::GetStations()
{
	if (this->tile != INVALID_TILE) {
		FindStationsAroundTiles(*this, &this->stations);
		this->tile = INVALID_TILE;
	}
	return &this->stations;
}

uint MoveGoodsToStation(CargoID type, uint amount, SourceType source_type, SourceID source_id, const StationList *all_stations)
{
	/* Return if nothing to do. Also the rounding below fails for 0. */
	if (amount == 0) return 0;

	Station *st1 = NULL;   // Station with best rating
	Station *st2 = NULL;   // Second best station
	uint best_rating1 = 0; // rating of st1
	uint best_rating2 = 0; // rating of st2

	for (Station * const *st_iter = all_stations->Begin(); st_iter != all_stations->End(); ++st_iter) {
		Station *st = *st_iter;

		/* Is the station reserved exclusively for somebody else? */
		if (st->town->exclusive_counter > 0 && st->town->exclusivity != st->owner) continue;

		if (st->goods[type].rating == 0) continue; // Lowest possible rating, better not to give cargo anymore

		if (_settings_game.order.selectgoods && !st->goods[type].HasVehicleEverTriedLoading()) continue; // Selectively servicing stations, and not this one

		if (!st->CanHandleCargo(type)) continue; // passengers on truck stop or freight on bus stop

		/* This station can be used, add it to st1/st2 */
		if (st1 == NULL || st->goods[type].rating >= best_rating1) {
			st2 = st1; best_rating2 = best_rating1; st1 = st; best_rating1 = st->goods[type].rating;
		} else if (st2 == NULL || st->goods[type].rating >= best_rating2) {
			st2 = st; best_rating2 = st->goods[type].rating;
		}
	}

	/* no stations around at all? */
	if (st1 == NULL) return 0;

	/* From now we'll calculate with fractal cargo amounts.
	 * First determine how much cargo we really have. */
	amount *= best_rating1 + 1;

	if (st2 == NULL) {
		/* only one station around */
		return UpdateStationWaiting(st1, type, amount, source_type, source_id);
	}

	/* several stations around, the best two (highest rating) are in st1 and st2 */
	assert(st1 != NULL);
	assert(st2 != NULL);
	assert(best_rating1 != 0 || best_rating2 != 0);

	/* Then determine the amount the worst station gets. We do it this way as the
	 * best should get a bonus, which in this case is the rounding difference from
	 * this calculation. In reality that will mean the bonus will be pretty low.
	 * Nevertheless, the best station should always get the most cargo regardless
	 * of rounding issues. */
	uint worst_cargo = amount * best_rating2 / (best_rating1 + best_rating2);
	assert(worst_cargo <= (amount - worst_cargo));

	/* And then send the cargo to the stations! */
	uint moved = UpdateStationWaiting(st1, type, amount - worst_cargo, source_type, source_id);
	/* These two UpdateStationWaiting's can't be in the statement as then the order
	 * of execution would be undefined and that could cause desyncs with callbacks. */
	return moved + UpdateStationWaiting(st2, type, worst_cargo, source_type, source_id);
}

void BuildOilRig(TileIndex tile)
{
	if (!Station::CanAllocateItem()) {
		DEBUG(misc, 0, "Can't allocate station for oilrig at 0x%X, reverting to oilrig only", tile);
		return;
	}

	if (!Dock::CanAllocateItem()) {
		DEBUG(misc, 0, "Can't allocate dock for oilrig at 0x%X, reverting to oilrig only", tile);
		return;
	}

	Station *st = new Station(tile);
	st->town = ClosestTownFromTile(tile);

	st->string_id = GenerateStationName(st, tile, STATIONNAMING_OILRIG);

	assert(IsIndustryTile(tile));
	DeleteAnimatedTile(tile);
	MakeOilrig(tile, st->index, GetWaterClass(tile));

	st->owner = OWNER_NONE;
	st->docks = new Dock(tile);
	st->dock_area = TileArea(tile, 1, 1);
	st->airport.type = AT_OILRIG;
	st->airport.Add(tile);
	st->facilities = FACIL_AIRPORT | FACIL_DOCK;
	st->build_date = _date;

	st->rect.Add(tile);

	st->UpdateVirtCoord();
	UpdateStationAcceptance(st, false);
	st->RecomputeIndustriesNear();
}

void DeleteOilRig(TileIndex tile)
{
	Station *st = Station::GetByTile(tile);

	MakeWaterKeepingClass(tile, OWNER_NONE);

	delete st->docks;
	st->docks = NULL;
	st->dock_area.Clear();
	st->airport.Clear();
	st->facilities &= ~(FACIL_AIRPORT | FACIL_DOCK);
	st->airport.flags = 0;

	st->AfterRemoveTile(tile);

	st->UpdateVirtCoord();
	st->RecomputeIndustriesNear();
	if (!st->IsInUse()) delete st;
}

static void ChangeTileOwner_Station(TileIndex tile, Owner old_owner, Owner new_owner)
{
	if (IsRoadStopTile(tile)) {
		for (RoadType rt = ROADTYPE_ROAD; rt < ROADTYPE_END; rt++) {
			/* Update all roadtypes, no matter if they are present */
			if (GetRoadOwner(tile, rt) == old_owner) {
				if (HasTileRoadType(tile, rt)) {
					/* A drive-through road-stop has always two road bits. No need to dirty windows here, we'll redraw the whole screen anyway. */
					Company::Get(old_owner)->infrastructure.road[rt] -= 2;
					if (new_owner != INVALID_OWNER) Company::Get(new_owner)->infrastructure.road[rt] += 2;
				}
				SetRoadOwner(tile, rt, new_owner == INVALID_OWNER ? OWNER_NONE : new_owner);
			}
		}
	}

	if (!IsTileOwner(tile, old_owner)) return;

	if (new_owner != INVALID_OWNER) {
		/* Update company infrastructure counts. Only do it here
		 * if the new owner is valid as otherwise the clear
		 * command will do it for us. No need to dirty windows
		 * here, we'll redraw the whole screen anyway.*/
		Company *old_company = Company::Get(old_owner);
		Company *new_company = Company::Get(new_owner);

		/* Update counts for underlying infrastructure. */
		switch (GetStationType(tile)) {
			case STATION_RAIL:
			case STATION_WAYPOINT:
				if (!IsStationTileBlocked(tile)) {
					old_company->infrastructure.rail[GetRailType(tile)]--;
					new_company->infrastructure.rail[GetRailType(tile)]++;
				}
				break;

			case STATION_BUS:
			case STATION_TRUCK:
				/* Road stops were already handled above. */
				break;

			case STATION_BUOY:
			case STATION_DOCK:
				if (GetWaterClass(tile) == WATER_CLASS_CANAL) {
					old_company->infrastructure.water--;
					new_company->infrastructure.water++;
				}
				break;

			default:
				break;
		}

		/* Update station tile count. */
		if (!IsBuoy(tile) && !IsAirport(tile)) {
			old_company->infrastructure.station--;
			new_company->infrastructure.station++;
		}

		/* for buoys, owner of tile is owner of water, st->owner == OWNER_NONE */
		SetTileOwner(tile, new_owner);
		InvalidateWindowClassesData(WC_STATION_LIST, 0);
	} else {
		if (IsDriveThroughStopTile(tile)) {
			/* Remove the drive-through road stop */
			DoCommand(tile, 1 | 1 << 8, (GetStationType(tile) == STATION_TRUCK) ? ROADSTOP_TRUCK : ROADSTOP_BUS, DC_EXEC | DC_BANKRUPT, CMD_REMOVE_ROAD_STOP);
			assert(IsNormalRoadTile(tile));
			/* Change owner of tile and all roadtypes */
			ChangeTileOwner(tile, old_owner, new_owner);
		} else {
			DoCommand(tile, 0, 0, DC_EXEC | DC_BANKRUPT, CMD_LANDSCAPE_CLEAR);
			/* Set tile owner of water under (now removed) buoy and dock to OWNER_NONE.
			 * Update owner of buoy if it was not removed (was in orders).
			 * Do not update when owned by OWNER_WATER (sea and rivers). */
			if ((IsWaterTile(tile) || IsBuoyTile(tile)) && IsTileOwner(tile, old_owner)) SetTileOwner(tile, OWNER_NONE);
		}
	}
}

/**
 * Check if a drive-through road stop tile can be cleared.
 * Road stops built on town-owned roads check the conditions
 * that would allow clearing of the original road.
 * @param tile road stop tile to check
 * @param flags command flags
 * @return true if the road can be cleared
 */
static bool CanRemoveRoadWithStop(TileIndex tile, DoCommandFlag flags)
{
	/* Yeah... water can always remove stops, right? */
	if (_current_company == OWNER_WATER) return true;

	RoadTypes rts = GetRoadTypes(tile);
	if (HasBit(rts, ROADTYPE_TRAM)) {
		Owner tram_owner = GetRoadOwner(tile, ROADTYPE_TRAM);
		if (tram_owner != OWNER_NONE && CheckOwnership(tram_owner).Failed()) return false;
	}
	if (HasBit(rts, ROADTYPE_ROAD)) {
		Owner road_owner = GetRoadOwner(tile, ROADTYPE_ROAD);
		if (road_owner != OWNER_TOWN) {
			if (road_owner != OWNER_NONE && CheckOwnership(road_owner).Failed()) return false;
		} else {
			if (CheckAllowRemoveRoad(tile, GetAnyRoadBits(tile, ROADTYPE_ROAD), OWNER_TOWN, ROADTYPE_ROAD, flags).Failed()) return false;
		}
	}

	return true;
}

/**
 * Clear a single tile of a station.
 * @param tile The tile to clear.
 * @param flags The DoCommand flags related to the "command".
 * @return The cost, or error of clearing.
 */
CommandCost ClearTile_Station(TileIndex tile, DoCommandFlag flags)
{
	if (flags & DC_AUTO) {
		switch (GetStationType(tile)) {
			default: break;
			case STATION_RAIL:     return_cmd_error(STR_ERROR_MUST_DEMOLISH_RAILROAD);
			case STATION_WAYPOINT: return_cmd_error(STR_ERROR_BUILDING_MUST_BE_DEMOLISHED);
			case STATION_AIRPORT:  return_cmd_error(STR_ERROR_MUST_DEMOLISH_AIRPORT_FIRST);
			case STATION_TRUCK:    return_cmd_error(HasTileRoadType(tile, ROADTYPE_TRAM) ? STR_ERROR_MUST_DEMOLISH_CARGO_TRAM_STATION_FIRST : STR_ERROR_MUST_DEMOLISH_TRUCK_STATION_FIRST);
			case STATION_BUS:      return_cmd_error(HasTileRoadType(tile, ROADTYPE_TRAM) ? STR_ERROR_MUST_DEMOLISH_PASSENGER_TRAM_STATION_FIRST : STR_ERROR_MUST_DEMOLISH_BUS_STATION_FIRST);
			case STATION_BUOY:     return_cmd_error(STR_ERROR_BUOY_IN_THE_WAY);
			case STATION_DOCK:     return_cmd_error(STR_ERROR_MUST_DEMOLISH_DOCK_FIRST);
			case STATION_OILRIG:
				SetDParam(1, STR_INDUSTRY_NAME_OIL_RIG);
				return_cmd_error(STR_ERROR_GENERIC_OBJECT_IN_THE_WAY);
		}
	}

	switch (GetStationType(tile)) {
		case STATION_RAIL:     return RemoveRailStation(tile, flags);
		case STATION_WAYPOINT: return RemoveRailWaypoint(tile, flags);
		case STATION_AIRPORT:  return RemoveAirport(tile, flags);
		case STATION_TRUCK:
			if (IsDriveThroughStopTile(tile) && !CanRemoveRoadWithStop(tile, flags)) {
				return_cmd_error(STR_ERROR_MUST_DEMOLISH_TRUCK_STATION_FIRST);
			}
			return RemoveRoadStop(tile, flags);
		case STATION_BUS:
			if (IsDriveThroughStopTile(tile) && !CanRemoveRoadWithStop(tile, flags)) {
				return_cmd_error(STR_ERROR_MUST_DEMOLISH_BUS_STATION_FIRST);
			}
			return RemoveRoadStop(tile, flags);
		case STATION_BUOY:     return RemoveBuoy(tile, flags);
		case STATION_DOCK:     return RemoveDock(tile, flags);
		default: break;
	}

	return CMD_ERROR;
}

static CommandCost TerraformTile_Station(TileIndex tile, DoCommandFlag flags, int z_new, Slope tileh_new)
{
	if (_settings_game.construction.build_on_slopes && AutoslopeEnabled()) {
		/* TODO: If you implement newgrf callback 149 'land slope check', you have to decide what to do with it here.
		 *       TTDP does not call it.
		 */
		if (GetTileMaxZ(tile) == z_new + GetSlopeMaxZ(tileh_new)) {
			switch (GetStationType(tile)) {
				case STATION_WAYPOINT:
				case STATION_RAIL: {
					DiagDirection direction = AxisToDiagDir(GetRailStationAxis(tile));
					if (!AutoslopeCheckForEntranceEdge(tile, z_new, tileh_new, direction)) break;
					if (!AutoslopeCheckForEntranceEdge(tile, z_new, tileh_new, ReverseDiagDir(direction))) break;
					return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
				}

				case STATION_AIRPORT:
					return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);

				case STATION_TRUCK:
				case STATION_BUS: {
					DiagDirection direction = GetRoadStopDir(tile);
					if (!AutoslopeCheckForEntranceEdge(tile, z_new, tileh_new, direction)) break;
					if (IsDriveThroughStopTile(tile)) {
						if (!AutoslopeCheckForEntranceEdge(tile, z_new, tileh_new, ReverseDiagDir(direction))) break;
					}
					return CommandCost(EXPENSES_CONSTRUCTION, _price[PR_BUILD_FOUNDATION]);
				}

				default: break;
			}
		}
	}
	return DoCommand(tile, 0, 0, flags, CMD_LANDSCAPE_CLEAR);
}

/**
 * Get flow for a station.
 * @param st Station to get flow for.
 * @return Flow for st.
 */
uint FlowStat::GetShare(StationID st) const
{
	uint32 prev = 0;
	for (SharesMap::const_iterator it = this->shares.begin(); it != this->shares.end(); ++it) {
		if (it->second == st) {
			return it->first - prev;
		} else {
			prev = it->first;
		}
	}
	return 0;
}

/**
 * Get a station a package can be routed to, but exclude the given ones.
 * @param excluded StationID not to be selected.
 * @param excluded2 Another StationID not to be selected.
 * @return A station ID from the shares map.
 */
StationID FlowStat::GetVia(StationID excluded, StationID excluded2) const
{
	if (this->unrestricted == 0) return INVALID_STATION;
	assert(!this->shares.empty());
	SharesMap::const_iterator it = this->shares.upper_bound(RandomRange(this->unrestricted));
	assert(it != this->shares.end() && it->first <= this->unrestricted);
	if (it->second != excluded && it->second != excluded2) return it->second;

	/* We've hit one of the excluded stations.
	 * Draw another share, from outside its range. */

	uint end = it->first;
	uint begin = (it == this->shares.begin() ? 0 : (--it)->first);
	uint interval = end - begin;
	if (interval >= this->unrestricted) return INVALID_STATION; // Only one station in the map.
	uint new_max = this->unrestricted - interval;
	uint rand = RandomRange(new_max);
	SharesMap::const_iterator it2 = (rand < begin) ? this->shares.upper_bound(rand) :
			this->shares.upper_bound(rand + interval);
	assert(it2 != this->shares.end() && it2->first <= this->unrestricted);
	if (it2->second != excluded && it2->second != excluded2) return it2->second;

	/* We've hit the second excluded station.
	 * Same as before, only a bit more complicated. */

	uint end2 = it2->first;
	uint begin2 = (it2 == this->shares.begin() ? 0 : (--it2)->first);
	uint interval2 = end2 - begin2;
	if (interval2 >= new_max) return INVALID_STATION; // Only the two excluded stations in the map.
	new_max -= interval2;
	if (begin > begin2) {
		Swap(begin, begin2);
		Swap(end, end2);
		Swap(interval, interval2);
	}
	rand = RandomRange(new_max);
	SharesMap::const_iterator it3 = this->shares.upper_bound(this->unrestricted);
	if (rand < begin) {
		it3 = this->shares.upper_bound(rand);
	} else if (rand < begin2 - interval) {
		it3 = this->shares.upper_bound(rand + interval);
	} else {
		it3 = this->shares.upper_bound(rand + interval + interval2);
	}
	assert(it3 != this->shares.end() && it3->first <= this->unrestricted);
	return it3->second;
}

/**
 * Reduce all flows to minimum capacity so that they don't get in the way of
 * link usage statistics too much. Keep them around, though, to continue
 * routing any remaining cargo.
 */
void FlowStat::Invalidate()
{
	assert(!this->shares.empty());
	SharesMap new_shares;
	uint i = 0;
	for (SharesMap::iterator it(this->shares.begin()); it != this->shares.end(); ++it) {
		new_shares[++i] = it->second;
		if (it->first == this->unrestricted) this->unrestricted = i;
	}
	this->shares.swap(new_shares);
	assert(!this->shares.empty() && this->unrestricted <= (--this->shares.end())->first);
}

/**
 * Change share for specified station. By specifing INT_MIN as parameter you
 * can erase a share. Newly added flows will be unrestricted.
 * @param st Next Hop to be removed.
 * @param flow Share to be added or removed.
 */
void FlowStat::ChangeShare(StationID st, int flow)
{
	/* We assert only before changing as afterwards the shares can actually
	 * be empty. In that case the whole flow stat must be deleted then. */
	assert(!this->shares.empty());

	uint removed_shares = 0;
	uint added_shares = 0;
	uint last_share = 0;
	SharesMap new_shares;
	for (SharesMap::iterator it(this->shares.begin()); it != this->shares.end(); ++it) {
		if (it->second == st) {
			if (flow < 0) {
				uint share = it->first - last_share;
				if (flow == INT_MIN || (uint)(-flow) >= share) {
					removed_shares += share;
					if (it->first <= this->unrestricted) this->unrestricted -= share;
					if (flow != INT_MIN) flow += share;
					last_share = it->first;
					continue; // remove the whole share
				}
				removed_shares += (uint)(-flow);
			} else {
				added_shares += (uint)(flow);
			}
			if (it->first <= this->unrestricted) this->unrestricted += flow;

			/* If we don't continue above the whole flow has been added or
			 * removed. */
			flow = 0;
		}
		new_shares[it->first + added_shares - removed_shares] = it->second;
		last_share = it->first;
	}
	if (flow > 0) {
		new_shares[last_share + (uint)flow] = st;
		if (this->unrestricted < last_share) {
			this->ReleaseShare(st);
		} else {
			this->unrestricted += flow;
		}
	}
	this->shares.swap(new_shares);
}

/**
 * Restrict a flow by moving it to the end of the map and decreasing the amount
 * of unrestricted flow.
 * @param st Station of flow to be restricted.
 */
void FlowStat::RestrictShare(StationID st)
{
	assert(!this->shares.empty());
	uint flow = 0;
	uint last_share = 0;
	SharesMap new_shares;
	for (SharesMap::iterator it(this->shares.begin()); it != this->shares.end(); ++it) {
		if (flow == 0) {
			if (it->first > this->unrestricted) return; // Not present or already restricted.
			if (it->second == st) {
				flow = it->first - last_share;
				this->unrestricted -= flow;
			} else {
				new_shares[it->first] = it->second;
			}
		} else {
			new_shares[it->first - flow] = it->second;
		}
		last_share = it->first;
	}
	if (flow == 0) return;
	new_shares[last_share + flow] = st;
	this->shares.swap(new_shares);
	assert(!this->shares.empty());
}

/**
 * Release ("unrestrict") a flow by moving it to the begin of the map and
 * increasing the amount of unrestricted flow.
 * @param st Station of flow to be released.
 */
void FlowStat::ReleaseShare(StationID st)
{
	assert(!this->shares.empty());
	uint flow = 0;
	uint next_share = 0;
	bool found = false;
	for (SharesMap::reverse_iterator it(this->shares.rbegin()); it != this->shares.rend(); ++it) {
		if (it->first < this->unrestricted) return; // Note: not <= as the share may hit the limit.
		if (found) {
			flow = next_share - it->first;
			this->unrestricted += flow;
			break;
		} else {
			if (it->first == this->unrestricted) return; // !found -> Limit not hit.
			if (it->second == st) found = true;
		}
		next_share = it->first;
	}
	if (flow == 0) return;
	SharesMap new_shares;
	new_shares[flow] = st;
	for (SharesMap::iterator it(this->shares.begin()); it != this->shares.end(); ++it) {
		if (it->second != st) {
			new_shares[flow + it->first] = it->second;
		} else {
			flow = 0;
		}
	}
	this->shares.swap(new_shares);
	assert(!this->shares.empty());
}

/**
 * Scale all shares from link graph's runtime to monthly values.
 * @param runtime Time the link graph has been running without compression.
 * @pre runtime must be greater than 0 as we don't want infinite flow values.
 */
void FlowStat::ScaleToMonthly(uint runtime)
{
	assert(runtime > 0);
	SharesMap new_shares;
	uint share = 0;
	for (SharesMap::iterator i = this->shares.begin(); i != this->shares.end(); ++i) {
		share = max(share + 1, i->first * 30 / runtime);
		new_shares[share] = i->second;
		if (this->unrestricted == i->first) this->unrestricted = share;
	}
	this->shares.swap(new_shares);
}

/**
 * Add some flow from "origin", going via "via".
 * @param origin Origin of the flow.
 * @param via Next hop.
 * @param flow Amount of flow to be added.
 */
void FlowStatMap::AddFlow(StationID origin, StationID via, uint flow)
{
	FlowStatMap::iterator origin_it = this->find(origin);
	if (origin_it == this->end()) {
		this->insert(std::make_pair(origin, FlowStat(via, flow)));
	} else {
		origin_it->second.ChangeShare(via, flow);
		assert(!origin_it->second.GetShares()->empty());
	}
}

/**
 * Pass on some flow, remembering it as invalid, for later subtraction from
 * locally consumed flow. This is necessary because we can't have negative
 * flows and we don't want to sort the flows before adding them up.
 * @param origin Origin of the flow.
 * @param via Next hop.
 * @param flow Amount of flow to be passed.
 */
void FlowStatMap::PassOnFlow(StationID origin, StationID via, uint flow)
{
	FlowStatMap::iterator prev_it = this->find(origin);
	if (prev_it == this->end()) {
		FlowStat fs(via, flow);
		fs.AppendShare(INVALID_STATION, flow);
		this->insert(std::make_pair(origin, fs));
	} else {
		prev_it->second.ChangeShare(via, flow);
		prev_it->second.ChangeShare(INVALID_STATION, flow);
		assert(!prev_it->second.GetShares()->empty());
	}
}

/**
 * Subtract invalid flows from locally consumed flow.
 * @param self ID of own station.
 */
void FlowStatMap::FinalizeLocalConsumption(StationID self)
{
	for (FlowStatMap::iterator i = this->begin(); i != this->end(); ++i) {
		FlowStat &fs = i->second;
		uint local = fs.GetShare(INVALID_STATION);
		if (local > INT_MAX) { // make sure it fits in an int
			fs.ChangeShare(self, -INT_MAX);
			fs.ChangeShare(INVALID_STATION, -INT_MAX);
			local -= INT_MAX;
		}
		fs.ChangeShare(self, -(int)local);
		fs.ChangeShare(INVALID_STATION, -(int)local);

		/* If the local share is used up there must be a share for some
		 * remote station. */
		assert(!fs.GetShares()->empty());
	}
}

/**
 * Delete all flows at a station for specific cargo and destination.
 * @param via Remote station of flows to be deleted.
 * @return IDs of source stations for which the complete FlowStat, not only a
 *         share, has been erased.
 */
StationIDStack FlowStatMap::DeleteFlows(StationID via)
{
	StationIDStack ret;
	for (FlowStatMap::iterator f_it = this->begin(); f_it != this->end();) {
		FlowStat &s_flows = f_it->second;
		s_flows.ChangeShare(via, INT_MIN);
		if (s_flows.GetShares()->empty()) {
			ret.Push(f_it->first);
			this->erase(f_it++);
		} else {
			++f_it;
		}
	}
	return ret;
}

/**
 * Restrict all flows at a station for specific cargo and destination.
 * @param via Remote station of flows to be restricted.
 */
void FlowStatMap::RestrictFlows(StationID via)
{
	for (FlowStatMap::iterator it = this->begin(); it != this->end(); ++it) {
		it->second.RestrictShare(via);
	}
}

/**
 * Release all flows at a station for specific cargo and destination.
 * @param via Remote station of flows to be released.
 */
void FlowStatMap::ReleaseFlows(StationID via)
{
	for (FlowStatMap::iterator it = this->begin(); it != this->end(); ++it) {
		it->second.ReleaseShare(via);
	}
}

/**
 * Get the sum of flows via a specific station from this GoodsEntry.
 * @param via Remote station to look for.
 * @return a FlowStat with all flows for 'via' added up.
 */
uint GoodsEntry::GetSumFlowVia(StationID via) const
{
	uint ret = 0;
	for (FlowStatMap::const_iterator i = this->flows.begin(); i != this->flows.end(); ++i) {
		ret += i->second.GetShare(via);
	}
	return ret;
}

extern const TileTypeProcs _tile_type_station_procs = {
	DrawTile_Station,           // draw_tile_proc
	GetSlopePixelZ_Station,     // get_slope_z_proc
	ClearTile_Station,          // clear_tile_proc
	NULL,                       // add_accepted_cargo_proc
	GetTileDesc_Station,        // get_tile_desc_proc
	GetTileRailwayStatus_Station,  // get_tile_railway_status_proc
	GetTileRoadStatus_Station,     // get_tile_road_status_proc
	GetTileWaterwayStatus_Station, // get_tile_waterway_status_proc
	ClickTile_Station,          // click_tile_proc
	AnimateTile_Station,        // animate_tile_proc
	TileLoop_Station,           // tile_loop_proc
	ChangeTileOwner_Station,    // change_tile_owner_proc
	NULL,                       // add_produced_cargo_proc
	GetFoundation_Station,      // get_foundation_proc
	TerraformTile_Station,      // terraform_tile_proc
};
