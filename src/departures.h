
/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file departures.h Functions related to departure boards. */

#ifndef DEPARTURES_H
#define DEPARTURES_H

#include "station_type.h"
#include "vehicle_base.h"
#include "date_func.h"

#include "core/smallvec_type.hpp"

#include <vector>

enum DepartureType {
        DT_DEPARTURE,
        DT_ARRIVAL,
};

enum DeparturesFrom {
        DF_STATION,
        DF_WAYPOINT,
};

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
void ShowDeparturesWindow(StationID station);

enum DeparturesInvalidateWindowData {
        DIWD_VEHICLE_NAME_CHANGED    = -1,
        DIWD_GROUP_NAME_CHANGED      = -2,
        DIWD_COMPANY_NAME_CHANGED    = -3,
        DIWD_DEPARTURES_FONT_CHANGED = -4,
        DIWD_STATION_NAME_CHANGED    = -5,
        DIWD_WAYPOINT_NAME_CHANGED   = -6,
        DIWD_CLOCK_TOGGLED           = -7,
};

struct ArrivalTime {
        Date date;
        DateFract date_fract;

        inline bool operator>(ArrivalTime that) const
        {
                return date > that.date || (date == that.date && date_fract > that.date_fract);
        }

        inline uint64 Ticks() const
        {
                return date * DAY_TICKS + date_fract;
        }

        inline ArrivalTime ExpectedArrival(int32 lateness_ticks) const
        {
                int64 ticks = (int64)date * DAY_TICKS;
                ticks += date_fract;
                ticks += lateness_ticks;
                ArrivalTime result = { (Date)(ticks / DAY_TICKS), (DateFract)(ticks % DAY_TICKS) };
                return result;
        }

        inline ArrivalTime& operator+=(uint16 ticks)
        {
                uint64 total_ticks = (uint64)date * DAY_TICKS;
                total_ticks += date_fract;
                total_ticks += ticks;
                date = total_ticks / DAY_TICKS;
                date_fract = total_ticks % DAY_TICKS;
                return *this;
        }

        inline ArrivalTime operator+(uint16 ticks) const
        {
                uint64 total_ticks = (uint64)date * DAY_TICKS;
                total_ticks += date_fract;
                total_ticks += ticks;
                return { (Date)(total_ticks / DAY_TICKS), (DateFract)(total_ticks % DAY_TICKS) };
        }

        inline Date DateAfter(int32 ticks) const
        {
                uint64 total_ticks = (uint64)date * DAY_TICKS;
                total_ticks += date_fract;
                total_ticks += ticks;
                return total_ticks / DAY_TICKS;
        }
};

enum VehicleStatus {
        VS_TRAVELLING,
        VS_ARRIVED,
        VS_GOING_TO_DEPOT,
};

enum DepartureStatus {
        DS_ON_TIME,
        DS_ARRIVED,
        DS_CANCELLED,
        DS_DELAYED,
        DS_EXPECTED,
};

struct CallingAt {
        StationID station;
        uint32 ticks_after_departure_start;
};

template <DepartureType Tdeparture_type>
struct DepartureInfo {
        SmallVector<CallingAt, 8> calling_at;
        StationID via;
        ArrivalTime arrival;
        uint16 wait_time;
        VehicleStatus vehicle_status;
        int32 lateness;
        const Vehicle *vehicle;
        VehicleType vehicle_type;
        /* 0 if this departure does not repeat, i.e. there is a conditional order in it */
        uint32 repeat_after;

        DepartureInfo(ArrivalTime arrival, uint16 wait_time, VehicleStatus vehicle_status, int32 lateness, const Vehicle *vehicle, VehicleType vehicle_type) : via(INVALID_STATION), arrival(arrival),
                wait_time(wait_time), vehicle_status(vehicle_status), lateness(lateness), vehicle(vehicle), vehicle_type(vehicle_type)
        {
        }

        void Reset(ArrivalTime arrival, uint16 wait_time, VehicleStatus vehicle_status)
        {
                this->calling_at.Clear();
                this->via = INVALID_STATION;
                this->arrival = arrival;
                this->wait_time = wait_time;
                this->vehicle_status = vehicle_status;
        }

        inline bool Repeats() const
        {
                return repeat_after != 0;
        }

        DepartureStatus Status() const
        {
                switch (vehicle_status) {
                        case VS_ARRIVED:
                                return DS_ARRIVED;

                        case VS_GOING_TO_DEPOT:
                                return DS_CANCELLED;

                        default:
                                Date expected = ExpectedArrival();
                                Date scheduled = ScheduledDeparture();
                                switch (Tdeparture_type) {
                                        case DT_DEPARTURE:
                                                if (expected > scheduled) {
                                                        return DS_EXPECTED;
                                                } else {
                                                        if (_date < scheduled) {
                                                                return DS_ON_TIME;
                                                        } else {
                                                                return DS_DELAYED;
                                                        }
                                                }

                                        case DT_ARRIVAL:
                                                if (expected > arrival.date) {
                                                        return DS_EXPECTED;
                                                } else {
                                                        if (_date < arrival.date) {
                                                                return DS_ON_TIME;
                                                        } else {
                                                                return DS_DELAYED;
                                                        }
                                                }
                                }
                }
        }

        inline Date ExpectedArrival() const
        {
                return arrival.DateAfter(lateness);
        }

        inline Date ScheduledDeparture() const
        {
                return arrival.DateAfter(wait_time);
        }

        inline bool operator>(const DepartureInfo &that) const
        {
                return this->arrival + this->wait_time > that.arrival + that.wait_time;
        }

        void ProgressToNextDeparture()
        {
                arrival += repeat_after;
                if (vehicle_status == VS_ARRIVED) {
                        vehicle_status = VS_TRAVELLING;
                }
        }
};

template <DepartureType Tdeparture_type>
using DepartureInfoList = std::vector<DepartureInfo<Tdeparture_type> >;

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
DepartureInfoList<Tdeparture_type> RecalculateDepartures(StationID station);

#endif /* DEPARTURES_H */
