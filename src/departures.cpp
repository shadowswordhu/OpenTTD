/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file departures.cpp Functions for departures. */

#include "stdafx.h"
#include "debug.h"
#include "departures.h"
#include "station_type.h"
#include "station_base.h"
#include "vehicle_base.h"
#include "strings_func.h"
#include "string_func.h"
#include "vehicle_func.h"
#include "vehiclelist.h"
#include "date_func.h"
#include "cargo_type.h"
#include "order_type.h"
#include "date_func.h"

#include "core/smallvec_type.hpp"

#include <vector>

struct FirstOrder {
        const Order *order;
        ArrivalTime arrival;
        bool arrived;
};

static inline bool OrderIsVia(const Order *order)
{
        return order->GetNonStopType() == ONSF_NO_STOP_AT_DESTINATION_STATION || order->GetNonStopType() == ONSF_NO_STOP_AT_ANY_STATION;
}

static inline bool OrderIsPickUp(const Order *order)
{
        return order->GetLoadType() != OLFB_NO_LOAD;
}

static inline bool OrderIsSetDown(const Order *order)
{
        return order->GetUnloadType() != OUFB_NO_UNLOAD;
}

static inline bool OrderIsFullUnload(const Order *order)
{
        return order->GetUnloadType() == OUFB_UNLOAD;
}

template <DeparturesFrom Tdepartures_from>
static FirstOrder FindFirstOrder(StationID station, const Vehicle *vehicle)
{
        const Order *order = vehicle->GetOrder(vehicle->cur_implicit_order_index % vehicle->GetNumOrders());

        bool arrived = vehicle->current_order.IsType(OT_LOADING);

        uint64 total_ticks = ((uint64)_date * DAY_TICKS) + _date_fract - vehicle->current_order_time;
        if (arrived) {
                total_ticks -= order->GetTravelTime();
                if (vehicle->lateness_counter < 0) {
                        /* If the vehicle arrived early, we must take into account its lateness
                         * in order to get the true order start time. */
                        total_ticks -= vehicle->lateness_counter;
                }
        } else {
                if (vehicle->lateness_counter > 0) {
                        total_ticks -= vehicle->lateness_counter;
                }
        }

        FirstOrder result = { NULL, { (Date)(total_ticks / DAY_TICKS), (DateFract)(total_ticks % DAY_TICKS) }, arrived };

        YearMonthDay ymd;
        ConvertDateToYMD(result.arrival.date, &ymd);
        if (arrived) {
                DEBUG(misc, 5, "The vehicle is loading, the order travel time was %d ticks, and the current order time is %d ticks, so the order start time has been calculated as %04d-%02d-%02d+%02d", order->GetTravelTime(), vehicle->current_order_time, ymd.year, ymd.month + 1, ymd.day, result.arrival.date_fract);
        } else {
                DEBUG(misc, 5, "The vehicle is not loading, and the current order time is %d ticks, so the order start time has been calculated as %04d-%02d-%02d+%02d", vehicle->current_order_time, ymd.year, ymd.month + 1, ymd.day, result.arrival.date_fract);
        }

        /* Find the first departure, i.e. an order to load cargo from the station departures are being calculated for. */
        for (byte i = 0; i < vehicle->GetNumOrders(); i++, order = order == vehicle->GetLastOrder() ? vehicle->GetFirstOrder() : order->next) {
                DEBUG(misc, 5, "Checking the next order to see if it is a departure");

                if (order->IsType(OT_CONDITIONAL) || order->GetTravelTime() == 0) {
                        DEBUG(misc, 4, "Vehicle has a conditional or untimetabled order prior to any departures");
                        break;
                }

                if (order->IsType(OT_GOTO_STATION)) {
                        bool via = OrderIsVia(order);

                        if (!via && order->GetWaitTime() == 0) {
                                DEBUG(misc, 4, "Vehicle has an order to go to a station with no scheduled wait time");
                                break;
                        }

                        StationID order_station = (StationID)order->GetDestination();
                        bool pick_up = OrderIsPickUp(order);

                        DEBUG(misc, 5, "Found an order to go to station %d %s, pick_up = %d, travel time = %d ticks, wait time = %d ticks", order_station, Station::Get(order_station)->name, pick_up, order->GetTravelTime(), order->GetWaitTime());

                        if (order_station == station && pick_up && !via) {
                                YearMonthDay ymd;
                                ConvertDateToYMD(result.arrival.date, &ymd);
                                DEBUG(misc, 4, "Found the vehicle's first departure from the station, which has an arrival time of %04d-%02d-%02d+%02d", ymd.year, ymd.month + 1, ymd.day, result.arrival.date_fract);
                                result.order = order;
                                result.arrival += order->GetTravelTime();
                                break;
                        } else {
                                DEBUG(misc, 6, "Skipping over order to go to station %d %s because it is a no-loading or via order", order_station, Station::Get(order_station)->name);
                        }
                }

                if (order->IsType(OT_GOTO_WAYPOINT) && Tdepartures_from == DF_WAYPOINT) {
                        StationID order_station = (StationID)order->GetDestination();

                        if (order_station == station) {
                                YearMonthDay ymd;
                                ConvertDateToYMD(result.arrival.date, &ymd);
                                DEBUG(misc, 4, "Found the vehicle's first departure from the waypoint, which has an arrival time of %04d-%02d-%02d+%02d", ymd.year, ymd.month + 1, ymd.day, result.arrival.date_fract);
                                result.order = order;
                                result.arrival += order->GetTravelTime();
                                break;
                        }
                }

                result.arrival += order->GetTravelTime();
                result.arrival += order->GetWaitTime();
                /* The vehicle has not arrived at the first departure from the station that departures are being calculated for. */
                result.arrived = false;
        }

        return result;
}

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
DepartureInfoList<Tdeparture_type> RecalculateDepartures(StationID station)
{
        DepartureInfoList<Tdeparture_type> result;

        YearMonthDay ymd;
        ConvertDateToYMD(_date, &ymd);
        DEBUG(misc, 3, "Calculating departures for station %d %s at %04d-%02d-%02d+%02d", station, Station::Get(station)->name, ymd.year, ymd.month + 1, ymd.day, _date_fract);

        for (VehicleType vehicle_type = VEH_BEGIN; vehicle_type < VEH_COMPANY_END; vehicle_type++) {
                DEBUG(misc, 4, "Calculating departures for vehicle type %d", vehicle_type);
                VehicleList vehicles;

                /* GenerateVehicleSortList ignores the company. */
                if (!GenerateVehicleSortList(&vehicles, VehicleListIdentifier(VL_STATION_LIST, vehicle_type, MAX_COMPANIES, station))) {
                        DEBUG(misc, 1, "Couldn't generate vehicle sort list for station %d %s and vehicle type %d", station, Station::Get(station)->name, vehicle_type);
                        /* TODO display an error? */
                        continue;
                }

                for (const Vehicle **v = vehicles.Begin(); v != vehicles.End(); v++) {
                        const Vehicle *vehicle = *v;
                        DEBUG(misc, 5, "Calculating departures for vehicle %s, which currently has a lateness of %d ticks", vehicle->name, vehicle->lateness_counter);

                        if (vehicle->IsStoppedInDepot()) {
                                continue;
                        }

                        bool cancelled = vehicle->current_order.IsType(OT_GOTO_DEPOT);

                        FirstOrder first_order = FindFirstOrder<Tdepartures_from>(station, vehicle);

                        if (first_order.order == NULL) {
                                DEBUG(misc, 4, "Vehicle has no departures from the station");
                                continue;
                        }

                        uint32 timetable_total_duration = first_order.order->GetTravelTime() + first_order.order->GetWaitTime();
                        uint32 ticks_after_departure_start = 0;

                        ArrivalTime start(first_order.arrival);

                        DepartureInfo<Tdeparture_type> departure(start, first_order.order->GetWaitTime(), cancelled ? VS_GOING_TO_DEPOT : (first_order.arrived ? VS_ARRIVED : VS_TRAVELLING), vehicle->lateness_counter, vehicle, vehicle_type);

                        byte departures_added = 0;

                        start += first_order.order->GetWaitTime();
                        const Order *order = first_order.order == vehicle->GetLastOrder() ? vehicle->GetFirstOrder() : first_order.order->next;
                        bool unloaded_everything = false;
                        StationID via = INVALID_STATION;
                        for (; order != first_order.order; order = order == vehicle->GetLastOrder() ? vehicle->GetFirstOrder() : order->next) {
                                DEBUG(misc, 5, "Processing the next order");
                                if (order->IsType(OT_CONDITIONAL) || order->GetTravelTime() == 0) {
                                        DEBUG(misc, 4, "Encountered a conditional or untimetabled order, giving up");
                                        timetable_total_duration = 0;
                                        break;
                                }

                                start += order->GetTravelTime();

                                if (order->IsType(OT_IMPLICIT)) {
                                        StationID order_station = (StationID)order->GetDestination();
                                        DEBUG(misc, 6, "Skipping over implicit order to go to station %d %s", order_station, Station::Get(order_station)->name);
                                        continue;
                                }

                                timetable_total_duration += order->GetTravelTime() + order->GetWaitTime();
                                ticks_after_departure_start += order->GetTravelTime() + order->GetWaitTime();

                                if (order->IsType(OT_GOTO_STATION)) {
                                        StationID order_station = (StationID)order->GetDestination();

                                        if (OrderIsVia(order)) {
                                                DEBUG(misc, 4, "Vehicle is going via station %d %s", order_station, Station::Get(order_station)->name);
                                                via = order_station;
                                                continue;
                                        } else if (order->GetWaitTime() == 0) {
                                                DEBUG(misc, 4, "Vehicle has an order to go to a station with no scheduled wait time");
                                                timetable_total_duration = 0;
                                                break;
                                        }

                                        bool already_seen = false;
                                        for (CallingAt *ca = departure.calling_at.Begin(); ca != departure.calling_at.End(); ca++) {
                                                if (ca->station == order_station) {
                                                        already_seen = true;
                                                        break;
                                                }
                                        }
                                        bool set_down = OrderIsSetDown(order);
                                        bool full_unload = OrderIsFullUnload(order);
                                        bool pick_up = OrderIsPickUp(order);

                                        DEBUG(misc, 5, "Found an order to go to station %d %s, already_seen = %d, set_down = %d, full_unload = %d, pick_up = %d, travel time = %d ticks, wait time = %d ticks", order_station, Station::Get(order_station)->name, already_seen, set_down, full_unload, pick_up, order->GetTravelTime(), order->GetWaitTime());

                                        switch (Tdeparture_type) {
                                                case DT_DEPARTURE:
                                                        if (station == order_station && pick_up) {
                                                                if (departure.calling_at.Length() != 0) {
                                                                        DEBUG(misc, 5, "Found a new departure");
                                                                        result.push_back(departure);
                                                                        departures_added++;
                                                                        ticks_after_departure_start = 0;
                                                                }

                                                                unloaded_everything = false;
                                                                departure.Reset(start, order->GetWaitTime(), cancelled ? VS_GOING_TO_DEPOT : VS_TRAVELLING);
                                                        } else if (set_down && !already_seen && !unloaded_everything) {
                                                                DEBUG(misc, 5, "Adding the order to go to station %d %s to the list of called at stations", order_station, Station::Get(order_station)->name);
                                                                *(departure.calling_at.Append()) = { order_station, ticks_after_departure_start - order->GetWaitTime() };
                                                                unloaded_everything = unloaded_everything | full_unload;
                                                                if (via == order_station && departure.via == INVALID_STATION) {
                                                                        departure.via = order_station;
                                                                        via = INVALID_STATION;
                                                                }
                                                        } else {
                                                                DEBUG(misc, 6, "Skipping over order to go to station %d %s", order_station, Station::Get(order_station)->name);
                                                                via = INVALID_STATION;
                                                        }
                                                        break;

                                                case DT_ARRIVAL:
                                                        if (station == order_station && set_down) {
                                                                if (departure.calling_at.Length() != 0) {
                                                                        DEBUG(misc, 5, "Found a new arrival");
                                                                        departure.arrival = start;
                                                                        result.push_back(departure);
                                                                        departures_added++;
                                                                        ticks_after_departure_start = 0;
                                                                }

                                                                departure.Reset(start, order->GetWaitTime(), cancelled ? VS_GOING_TO_DEPOT : VS_TRAVELLING);
                                                        } else if (pick_up) {
                                                                DEBUG(misc, 5, "Adding the order to go to station %d %s to the list of called at stations", order_station, Station::Get(order_station)->name);

                                                                int existing_index = -1;
                                                                for (uint index = 0; index < departure.calling_at.Length(); index++) {
                                                                        if (departure.calling_at[index].station == order_station) {
                                                                                existing_index = index;
                                                                                break;
                                                                        }
                                                                }
                                                                if (existing_index != -1) {
                                                                        departure.calling_at.ErasePreservingOrder(existing_index);
                                                                }

                                                                if (full_unload) {
                                                                        departure.calling_at.Clear();
                                                                        departure.via = INVALID_STATION;
                                                                }

                                                                *(departure.calling_at.Append()) = { order_station, ticks_after_departure_start - order->GetWaitTime() };
                                                                if (via == order_station && departure.via == INVALID_STATION) {
                                                                        departure.via = order_station;
                                                                        via = INVALID_STATION;
                                                                }
                                                        } else {
                                                                DEBUG(misc, 6, "Skipping over order to go to station %d %s", order_station, Station::Get(order_station)->name);
                                                                if (full_unload) {
                                                                        departure.calling_at.Clear();
                                                                }
                                                                via = INVALID_STATION;
                                                        }
                                                        break;
                                        }
                                }

                                if (order->IsType(OT_GOTO_WAYPOINT) && Tdepartures_from == DF_WAYPOINT) {
                                        if (departure.calling_at.Length() != 0) {
                                                DEBUG(misc, 5, "Found a new departure from/arrival at the waypoint");
                                                result.push_back(departure);
                                                departures_added++;
                                                ticks_after_departure_start = 0;
                                        }

                                        unloaded_everything = false;
                                        departure.Reset(start, order->GetWaitTime(), cancelled ? VS_GOING_TO_DEPOT : VS_TRAVELLING);
                                }

                                start += order->GetWaitTime();
                        }
                        if (departure.calling_at.Length() != 0) {
                                switch (Tdeparture_type) {
                                        case DT_DEPARTURE:
                                                break;

                                        case DT_ARRIVAL:
                                                departure.arrival = first_order.arrival;
                                                departure.vehicle_status = cancelled ? VS_GOING_TO_DEPOT : (first_order.arrived ? VS_ARRIVED : VS_TRAVELLING);
                                                break;
                                }
                                result.push_back(departure);
                                departures_added++;
                        }
                        DEBUG(misc, 5, "Finished finding departures, found %d", departures_added);

                        /* Set the repeat time for each of the departures. */
                        for (typename DepartureInfoList<Tdeparture_type>::reverse_iterator it = result.rbegin(); departures_added > 0; departures_added--, it++) {
                                it->repeat_after = timetable_total_duration;
                        }

                        DEBUG(misc, 4, "The timetable total duration is %d", timetable_total_duration);
                }
        }

        for (typename DepartureInfoList<Tdeparture_type>::iterator it = result.begin(); it != result.end(); it++) {
                YearMonthDay arrival_ymd;
                ConvertDateToYMD(it->arrival.date, &arrival_ymd);

                Date departure = it->ScheduledDeparture();
                YearMonthDay departure_ymd;
                ConvertDateToYMD(departure, &departure_ymd);
                DEBUG(misc, 4, "Vehicle %s type %d arrival at %04d-%02d-%02d+%02d departure at %04d-%02d-%02d repeats after %d ticks with lateness %d ticks and vehicle status %d", it->vehicle->name, it->vehicle_type, arrival_ymd.year, arrival_ymd.month + 1, arrival_ymd.day, it->arrival.date_fract, departure_ymd.year, departure_ymd.month + 1, departure_ymd.day, it->repeat_after, it->lateness, it->vehicle_status);
                for (CallingAt *calling_at = it->calling_at.Begin(); calling_at != it->calling_at.End(); calling_at++) {
                        DEBUG(misc, 4, "%d %s", calling_at->station, Station::Get(calling_at->station)->name);
                }
        }

        return result;
}

template DepartureInfoList<DT_DEPARTURE> RecalculateDepartures<DT_DEPARTURE, DF_STATION>(StationID station);
template DepartureInfoList<DT_DEPARTURE> RecalculateDepartures<DT_DEPARTURE, DF_WAYPOINT>(StationID station);
template DepartureInfoList<DT_ARRIVAL> RecalculateDepartures<DT_ARRIVAL, DF_STATION>(StationID station);
template DepartureInfoList<DT_ARRIVAL> RecalculateDepartures<DT_ARRIVAL, DF_WAYPOINT>(StationID station);
