/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file departures_gui.cpp The GUI for departures. */

#include "stdafx.h"
#include "debug.h"
#include "departures.h"
#include "station_type.h"
#include "station_base.h"
#include "gui.h"
#include "vehicle_base.h"
#include "window_gui.h"
#include "widgets/departures_widget.h"
#include "strings_func.h"
#include "string_func.h"
#include "vehicle_func.h"
#include "vehiclelist.h"
#include "date_func.h"
#include "cargo_type.h"
#include "order_type.h"
#include "date_func.h"
#include "vehicle_gui.h"
#include "company_base.h"

#include <algorithm>
#include <bitset>
#include <queue>
#include <unordered_map>
#include <vector>
#include "core/smallvec_type.hpp"

template <DepartureType Tdeparture_type>
struct PrintableDeparture {
        Date scheduled_arrival;
        Date scheduled_departure;
        Date expected;
        DepartureStatus status;
        const SmallVector<CallingAt, 8> *calling_at;
        size_t destination_index;
        StationID via;
        const Vehicle *vehicle;
        VehicleType vehicle_type;

        PrintableDeparture(const DepartureInfo<Tdeparture_type> *di, size_t destination_index, StationID via) : scheduled_arrival(di->arrival.date),
        scheduled_departure(di->ScheduledDeparture()),
        expected(di->ExpectedArrival()),
        status(di->Status()),
        calling_at(&di->calling_at),
        destination_index(destination_index),
        via(via),
        vehicle(di->vehicle),
        vehicle_type(di->vehicle_type)
        {
        }
};

template <DepartureType Tdeparture_type>
using PrintableDepartureList = std::vector<PrintableDeparture<Tdeparture_type> >;

typedef std::bitset<5> VehicleTypes;

enum ShowVehicleType {
        SVT_TRAINS,
        SVT_BUSES,
        SVT_LORRIES,
        SVT_SHIPS,
        SVT_PLANES,
};

typedef std::bitset<3> Labels;

enum ShowLabel {
        SL_VEHICLES,
        SL_GROUPS,
        SL_COMPANIES,
};

static uint16 days_of_departures = DAYS_IN_LEAP_YEAR * 1;

template <DepartureType Tdeparture_type>
static inline bool ShowDeparture(const DepartureInfo<Tdeparture_type> &di, const VehicleTypes &vehicle_types)
{
        switch (di.vehicle_type) {
                case VEH_TRAIN:
                        return vehicle_types[SVT_TRAINS];

                case VEH_ROAD: {
                        bool pax = IsCargoInClass(di.vehicle->cargo_type, CC_PASSENGERS);
                        return (pax && vehicle_types[SVT_BUSES]) || (!pax && vehicle_types[SVT_LORRIES]);
                }

                case VEH_SHIP:
                        return vehicle_types[SVT_SHIPS];

                case VEH_AIRCRAFT:
                        return vehicle_types[SVT_PLANES];

                default:
                        return false;
        }
}

template <DepartureType Tdeparture_type>
static uint16 NumberOfDepartures(const DepartureInfoList<Tdeparture_type> &departures, const VehicleTypes &vehicle_types)
{
        uint16 result = 0;
        DEBUG(misc, 3, "Calculating the number of departures that will be shown");

        Date limit = (_date + days_of_departures) * DAY_TICKS;
        for (typename DepartureInfoList<Tdeparture_type>::const_iterator it = departures.begin(); it != departures.end(); it++) {
                int64 scheduled = it->arrival.Ticks();
                switch (Tdeparture_type) {
                        case DT_DEPARTURE:
                                scheduled += it->wait_time;
                                break;

                        case DT_ARRIVAL:
                                break;
                }
                if (scheduled < limit && ShowDeparture<Tdeparture_type>(*it, vehicle_types)) {
                        DEBUG(misc, 4, "Processing a departure that arrives before the last date that will be shown");
                        result += 1;
                        if (it->repeat_after != 0) {
                                uint64 ticks = limit - scheduled;
                                result += (limit - scheduled) / it->repeat_after;
                                DEBUG(misc, 4, "This departure repeats every %d ticks, there are %llu ticks between its first arrival and the last date that will be shown, so it will result in %llu departures to show", it->repeat_after, ticks, ticks / it->repeat_after);
                        } else {
                                DEBUG(misc, 4, "This departure does not repeat");
                        }
                }
        }

        DEBUG(misc, 3, "%d departures will be shown", result);
        return result;
}

template <DepartureType Tdeparture_type>
struct DepartureAfter : std::binary_function<DepartureInfo<Tdeparture_type>*, DepartureInfo<Tdeparture_type>*, bool> {
        bool operator()(const DepartureInfo<Tdeparture_type> *lhs, const DepartureInfo<Tdeparture_type> *rhs) const
        {
                switch (Tdeparture_type) {
                        case DT_DEPARTURE:
                                return *lhs > *rhs;

                        case DT_ARRIVAL:
                                return lhs->arrival > rhs->arrival;
                }
        }
};

struct DestinationStationArrival {
        uint64 start_ticks;
        uint32 after_ticks;
        uint32 every_ticks;
};
typedef SmallVector<DestinationStationArrival, 8> DestinationStationArrivalList;

template <DepartureType Tdeparture_type, typename Tordering>
static PrintableDepartureList<Tdeparture_type> CreatePrintableDepartures(StationID station, DepartureInfoList<Tdeparture_type> &departures, VehicleTypes &vehicle_types, uint skip, uint len)
{
        DEBUG(misc, 4, "Calculating printable departure list for %d %s, skipping %d and calculating up to %d departures", station, Station::Get(station)->name, skip, len);

        PrintableDepartureList<Tdeparture_type> result;

        std::unordered_map<StationID, DestinationStationArrivalList> arrival_times;
        switch (Tdeparture_type) {
                case DT_DEPARTURE:
                        for (typename DepartureInfoList<Tdeparture_type>::iterator it = departures.begin(); it != departures.end(); it++) {
                                const DepartureInfo<Tdeparture_type> &di = *it;
                                for (const CallingAt *calling_at = di.calling_at.Begin(); calling_at != di.calling_at.End(); calling_at++) {
                                        if (vehicle_types[di.vehicle_type]) {
                                                DestinationStationArrivalList &arrivals = arrival_times[calling_at->station];
                                                (*arrivals.Append()) = { di.arrival.Ticks() + di.wait_time, calling_at->ticks_after_departure_start, di.repeat_after };
                                        }
                                }
                        }
                        break;

                case DT_ARRIVAL:
                        break;
        }

        std::priority_queue<DepartureInfo<Tdeparture_type>*, std::vector<DepartureInfo<Tdeparture_type>*>, Tordering> q;

        Date limit = _date + days_of_departures;

        for (typename DepartureInfoList<Tdeparture_type>::iterator it = departures.begin(); it != departures.end(); it++) {
                if (it->ScheduledDeparture() < limit && ShowDeparture<Tdeparture_type>(*it, vehicle_types)) {
                        q.push(&*it);
                }
        }

        for (uint i = 0; i < skip && !q.empty(); i++) {
                DepartureInfo<Tdeparture_type> *di = q.top();
                q.pop();

                if (di->Repeats()) {
                        di->ProgressToNextDeparture();
                        if (di->ScheduledDeparture() < limit) {
                                q.push(di);
                        }
                }
        }

        for (uint i = 0; i < len && !q.empty(); i++) {
                DepartureInfo<Tdeparture_type> *di = q.top();
                q.pop();

                Date departure = di->ScheduledDeparture();
                YearMonthDay scheduled_ymd;
                ConvertDateToYMD(departure, &scheduled_ymd);
                YearMonthDay expected_ymd;
                ConvertDateToYMD(di->ExpectedArrival(), &expected_ymd);
                DEBUG(misc, 5, "Adding departure for vehicle %s at %04d-%02d-%02d (%02d), expected at %04d-%02d-%02d, status %d, calling at:", di->vehicle->name, scheduled_ymd.year, scheduled_ymd.month + 1, scheduled_ymd.day, departure, expected_ymd.year, expected_ymd.month + 1, expected_ymd.day, di->Status());
                for (CallingAt *ca = di->calling_at.Begin(); ca != di->calling_at.End(); ca++) {
                        DEBUG(misc, 5, "%d %s", ca->station, Station::Get(ca->station)->name);
                }
                StationID via = di->via;
                size_t destination_index;
                switch (Tdeparture_type) {
                        case DT_DEPARTURE:
                                for (destination_index = di->calling_at.Length() - 1; destination_index > 0; destination_index--) {
                                        bool found_later_departure_that_arrives_earlier = false;
                                        CallingAt &calling_at = di->calling_at[destination_index];
                                        if (calling_at.station == via) {
                                                via = INVALID_STATION;
                                        }
                                        uint64 departure_ticks = di->arrival.Ticks() + di->wait_time;
                                        uint64 calling_at_ticks = departure_ticks + calling_at.ticks_after_departure_start;
                                        std::unordered_map<StationID, DestinationStationArrivalList>::iterator it = arrival_times.find(calling_at.station);
                                        if (it != arrival_times.end()) {
                                                DestinationStationArrivalList &arrivals = it->second;
                                                for (DestinationStationArrival *arrival = arrivals.Begin(); arrival != arrivals.End(); arrival++) {
                                                        if (arrival->every_ticks == 0) {
                                                                if (arrival->start_ticks >= departure_ticks && arrival->start_ticks + arrival->after_ticks < calling_at_ticks) {
                                                                        found_later_departure_that_arrives_earlier = true;
                                                                        break;
                                                                }
                                                        } else {
                                                                uint64 first_departure_time_after = arrival->start_ticks;
                                                                int64 diff = (int64)departure_ticks - arrival->start_ticks;
                                                                if (diff > 0) {
                                                                        first_departure_time_after = arrival->start_ticks + ((diff + arrival->every_ticks - 1) / arrival->every_ticks) * arrival->every_ticks;
                                                                }
                                                                if (first_departure_time_after + arrival->after_ticks < calling_at_ticks) {
                                                                        found_later_departure_that_arrives_earlier = true;
                                                                        break;
                                                                }
                                                        }
                                                }
                                        }
                                        if (!found_later_departure_that_arrives_earlier) {
                                                break;
                                        }
                                }
                                break;

                        case DT_ARRIVAL:
                                destination_index = 0;
                                break;
                }
                result.push_back(PrintableDeparture<Tdeparture_type>(di, destination_index, via));

                if (di->Repeats()) {
                        di->ProgressToNextDeparture();

                        if (di->ScheduledDeparture() < limit) {
                                YearMonthDay ymd;
                                ConvertDateToYMD(di->ScheduledDeparture(), &ymd);
                                DEBUG(misc, 5, "This departure will repeat at %04d-%02d-%02d, putting it into the queue again", ymd.year, ymd.month + 1, ymd.day);
                                q.push(di);
                        }
                } else {
                        DEBUG(misc, 5, "This departure will not repeat, not putting it into the queue again");
                }
        }

        return result;
}

static uint MaxDateWidth(bool include_arrival)
{
        if (include_arrival) {
                SetDParamMaxValue(0, MAX_YEAR * DAYS_IN_YEAR);
                SetDParamMaxValue(1, MAX_YEAR * DAYS_IN_YEAR);
                return GetStringBoundingBox(STR_DEPARTURES_ARRIVAL_AND_DEPARTURE_DATES).width;
        } else {
                SetDParamMaxValue(0, MAX_YEAR * DAYS_IN_YEAR);
                return GetStringBoundingBox(STR_DEPARTURES_DEPARTURE_DATE).width;
        }
}

static uint MaxStatusWidth()
{
        uint result = 0;

        SetDParamMaxValue(0, MAX_YEAR * DAYS_IN_YEAR);
        result = GetStringBoundingBox(STR_DEPARTURES_EXPECTED).width;

        result = max(GetStringBoundingBox(STR_DEPARTURES_ON_TIME).width, result);
        result = max(GetStringBoundingBox(STR_DEPARTURES_DELAYED).width, result);
        result = max(GetStringBoundingBox(STR_DEPARTURES_CANCELLED).width, result);

        return result;
}

static uint MaxIconWidth()
{
        return GetStringBoundingBox(STR_DEPARTURES_ICON_PLANE).width;
}

static uint CallingAtWidth()
{
        return GetStringBoundingBox(_settings_client.gui.larger_departures_font ? STR_DEPARTURES_CALLING_AT_LARGE : STR_DEPARTURES_CALLING_AT).width;
}

static uint MaxDestinationWidth()
{
        uint result = 0;

        for (size_t s = 0; s < Station::GetPoolSize(); s++) {
                if (Station::IsValidID(s)) {
                        SetDParam(0, s);
                        SetDParam(1, STR_DEPARTURES_STATION_PLANE);
                        result = max(result, GetStringBoundingBox(STR_DEPARTURES_DESTINATION_VIA).width);
                }
        }

        return result;
}

static uint MaxVehicleWidth()
{
        uint result = 0;

        for (size_t v = 0; v < Vehicle::GetPoolSize(); v++) {
                if (Vehicle::IsValidID(v)) {
                        SetDParam(0, v);
                        result = max(result, GetStringBoundingBox(STR_DEPARTURES_VEHICLE).width);
                }
        }

        return result;
}

static uint MaxGroupWidth()
{
        uint result = 0;

        for (size_t g = 0; g < Group::GetPoolSize(); g++) {
                if (Group::IsValidID(g)) {
                        SetDParam(0, g);
                        result = max(result, GetStringBoundingBox(STR_DEPARTURES_GROUP).width);
                }
        }

        return result;
}

static uint MaxCompanyWidth()
{
        uint result = 0;

        for (size_t c = 0; c < Company::GetPoolSize(); c++) {
                if (Company::IsValidID(c)) {
                        SetDParam(0, c);
                        result = max(result, GetStringBoundingBox(STR_DEPARTURES_COMPANY).width);
                }
        }

        return result;
}

template <DeparturesFrom Tdepartures_from>
static uint MaxEmptyWidth()
{
        switch (Tdepartures_from) {
                case DF_STATION:
                        return GetStringBoundingBox(STR_DEPARTURES_EMPTY).width;

                case DF_WAYPOINT:
                        return GetStringBoundingBox(STR_DEPARTURES_WAYPOINT_EMPTY).width;
        }
}

static FontSize CallingAtFontSize()
{
        return _settings_client.gui.larger_departures_font ? FS_NORMAL : FS_SMALL;
}

static uint CallingAtFontHeight()
{
        return _settings_client.gui.larger_departures_font ? FONT_HEIGHT_NORMAL : FONT_HEIGHT_SMALL;
}

static uint EntryHeight()
{
        return FONT_HEIGHT_NORMAL + CallingAtFontHeight() + 4;
}

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
struct DeparturesWindow : public Window {
        StationID station;
        Scrollbar *vscroll;
        VehicleTypes vehicle_types;
        Labels labels;
        uint entry_height;
        uint date_width;
        uint status_width;
        uint icon_width;
        uint calling_at_width;
        uint vehicle_width;
        uint group_width;
        uint company_width;
        uint destination_width;
        uint empty_width;
        uint number_of_departures;
        uint16 skip;
        uint16 capacity;
        uint64 tick_count;
        bool show_scheduled_arrivals;
        SmallVector<const Vehicle*, 8> vehicles;

        DepartureInfoList<Tdeparture_type> departure_info;
        PrintableDepartureList<Tdeparture_type> departures;

        void DrawDeparturesList(const Rect &r) const;

        DeparturesWindow(WindowDesc *desc, WindowNumber window_number) : Window(desc),
                station((StationID)window_number),
                vehicle_types("11111"),
                labels("000"),
                show_scheduled_arrivals(false)
        {
                this->CreateNestedTree();
                this->vscroll = this->GetScrollbar(WID_DV_SCROLLBAR);
                this->FinishInitNested(window_number);
                this->owner = Station::Get(window_number)->owner;

                this->LowerWidget(WID_DV_SHOW_TRAINS);
                this->LowerWidget(WID_DV_SHOW_BUSES);
                this->LowerWidget(WID_DV_SHOW_LORRIES);
                this->LowerWidget(WID_DV_SHOW_SHIPS);
                this->LowerWidget(WID_DV_SHOW_PLANES);
        }

        virtual void OnTick() {
                if (_pause_mode == PM_UNPAUSED) {
                        tick_count++;
                }

                departure_info = RecalculateDepartures<Tdeparture_type, Tdepartures_from>(station);

                number_of_departures = NumberOfDepartures<Tdeparture_type>(departure_info, vehicle_types);
                this->SetWidgetDirty(WID_DV_LIST);
                this->vscroll->SetCount(number_of_departures);
                skip = this->vscroll->GetPosition();
                capacity = this->vscroll->GetCapacity();

                departures = CreatePrintableDepartures<Tdeparture_type, DepartureAfter<Tdeparture_type> >(station, departure_info, vehicle_types, skip, capacity);

                vehicles.Clear();
                for (typename PrintableDepartureList<Tdeparture_type>::const_iterator it = departures.begin(); it != departures.end(); it++) {
                        *vehicles.Append() = it->vehicle;
                }
        }

        virtual void UpdateWidgetSize(int widget, Dimension *size, const Dimension &padding, Dimension *fill, Dimension *resize)
        {
                switch (widget) {
                        case WID_DV_LIST: {
                                entry_height = EntryHeight();
                                resize->height = entry_height;
                                size->height = 2 * resize->height;

                                date_width = MaxDateWidth(show_scheduled_arrivals);
                                status_width = MaxStatusWidth();
                                icon_width = MaxIconWidth();
                                calling_at_width = CallingAtWidth();
                                vehicle_width = MaxVehicleWidth();
                                group_width = MaxGroupWidth();
                                company_width = MaxCompanyWidth();
                                destination_width = MaxDestinationWidth();
                                empty_width = MaxEmptyWidth<Tdepartures_from>();

                                uint top_row_width = 3;
                                top_row_width += 3 + date_width;
                                top_row_width += 3 + status_width;
                                top_row_width += 3 + icon_width;
                                if (labels[SL_VEHICLES]) {
                                        top_row_width += 3 + vehicle_width;
                                }
                                if (labels[SL_GROUPS]) {
                                        top_row_width += 3 + group_width;
                                }
                                if (labels[SL_COMPANIES]) {
                                        top_row_width += 3 + company_width;
                                }
                                top_row_width += 3 + destination_width;

                                uint bottom_row_width = 3;
                                bottom_row_width += 3 + max(calling_at_width, empty_width);

                                size->width = max(size->width, max(bottom_row_width, top_row_width));
                                break;
                        }

                        default:
                                return;
                }
        }

        virtual void OnInvalidateData(int data = 0, bool gui_scope = true)
        {
                switch (data) {
                        case DIWD_VEHICLE_NAME_CHANGED:
                                if (labels[SL_VEHICLES]) {
                                        this->ReInit();
                                }
                                break;

                        case DIWD_GROUP_NAME_CHANGED:
                                if (labels[SL_GROUPS]) {
                                        this->ReInit();
                                }
                                break;

                        case DIWD_COMPANY_NAME_CHANGED:
                                if (labels[SL_COMPANIES]) {
                                        this->ReInit();
                                }
                                break;

                        case DIWD_DEPARTURES_FONT_CHANGED:
                        case DIWD_STATION_NAME_CHANGED:
                        case DIWD_CLOCK_TOGGLED:
                                this->ReInit();
                                break;

                        case DIWD_WAYPOINT_NAME_CHANGED:
                                if (Tdepartures_from == DF_WAYPOINT) {
                                        this->ReInit();
                                }
                                break;
                }
        }

        void VehicleTypeToggled(int widget, ShowVehicleType vehicle_type)
        {
                vehicle_types.flip(vehicle_type);
                if (vehicle_types[vehicle_type]) {
                        this->LowerWidget(widget);
                } else {
                        this->RaiseWidget(widget);
                }
                this->ReInit();
        }

        void LabelToggled(int widget, ShowLabel label)
        {
                labels.flip(label);
                if (labels[label]) {
                        this->LowerWidget(widget);
                } else {
                        this->RaiseWidget(widget);
                }
                this->ReInit();
        }

        virtual void OnClick(Point pt, int widget, int click_count)
        {
                switch (widget) {
                        default: return;

                        case WID_DV_SHOW_ARRIVAL_TIME:
                                show_scheduled_arrivals = !show_scheduled_arrivals;
                                if (show_scheduled_arrivals) {
                                        this->LowerWidget(WID_DV_SHOW_ARRIVAL_TIME);
                                } else {
                                        this->RaiseWidget(WID_DV_SHOW_ARRIVAL_TIME);
                                }
                                this->ReInit();
                                return;

                        case WID_DV_SHOW_TRAINS:
                                VehicleTypeToggled(widget, SVT_TRAINS);
                                return;

                        case WID_DV_SHOW_BUSES:
                                VehicleTypeToggled(widget, SVT_BUSES);
                                return;

                        case WID_DV_SHOW_LORRIES:
                                VehicleTypeToggled(widget, SVT_LORRIES);
                                return;

                        case WID_DV_SHOW_SHIPS:
                                VehicleTypeToggled(widget, SVT_SHIPS);
                                return;

                        case WID_DV_SHOW_PLANES:
                                VehicleTypeToggled(widget, SVT_PLANES);
                                return;

                        case WID_DV_SHOW_VEHICLE:
                                LabelToggled(widget, SL_VEHICLES);
                                return;

                        case WID_DV_SHOW_GROUP:
                                LabelToggled(widget, SL_GROUPS);
                                return;

                        case WID_DV_SHOW_COMPANY:
                                LabelToggled(widget, SL_COMPANIES);
                                return;

                        case WID_DV_LIST:
                                /* Calculate which row was clicked. */
                                uint32 row = this->vscroll->GetScrolledRowFromWidget(pt.y, this, WID_DV_LIST);
                                if (row >= this->vscroll->GetCapacity()) {
                                        return; // click out of bounds
                                }

                                row += this->vscroll->GetPosition();

                                if (row >= vehicles.Length()) {
                                        return;
                                }

                                ShowVehicleViewWindow(vehicles[row]);
                                return;
                }

        }

        virtual void SetStringParameters(int widget) const
        {
                switch (widget) {
                        case WID_DV_CAPTION:
                                const Station *st = Station::Get(this->window_number);
                                SetDParam(0, st->index);
                                break;
                }
        }

        virtual void OnResize()
        {
                this->vscroll->SetCapacityFromWidget(this, WID_DV_LIST);
                this->GetWidget<NWidgetCore>(WID_DV_LIST)->widget_data = (this->vscroll->GetCapacity() << MAT_ROW_START) + (1 << MAT_COL_START);
        }

        virtual void OnPaint()
        {
                this->DrawWidgets();
        }

        virtual void DrawWidget(const Rect &r, int widget) const
        {
                switch (widget) {
                        case WID_DV_LIST:
                                DrawDeparturesList(r);
                                break;
                }
        }
};

static const NWidgetPart _nested_arrivals_button_widgets[] = {
        NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_ARRIVAL_TIME), SetMinimalSize(6, 12), SetResize(1, 0), SetFill(1, 1), SetDataTip(STR_DEPARTURES_ARRIVAL_TIME_BUTTON, STR_DEPARTURES_ARRIVAL_TIME_TOOLTIP),
};

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
static NWidgetBase *ArrivalsButton(int *biggest_index)
{
        if (Tdeparture_type == DT_DEPARTURE && Tdepartures_from == DF_STATION) {
                return MakeNWidgets(_nested_arrivals_button_widgets, lengthof(_nested_arrivals_button_widgets), biggest_index, new NWidgetHorizontal());
        } else {
                return MakeNWidgets(NULL, 0, biggest_index, new NWidgetHorizontal());
        }
}

#define WIDGET_STRING(PART) (\
        Tdeparture_type == DT_DEPARTURE ? \
                (Tdepartures_from == DF_STATION ? STR_DEPARTURES_ ## PART : STR_ARRIVALS_WAYPOINT_ ## PART) :\
                (Tdepartures_from == DF_STATION ? STR_ARRIVALS_ ## PART : STR_ARRIVALS_WAYPOINT_ ## PART)\
        )

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
static const NWidgetPart _nested_departures_widgets[] = {
        NWidget(NWID_HORIZONTAL),
                NWidget(WWT_CLOSEBOX, COLOUR_GREY),
                NWidget(WWT_CAPTION, COLOUR_GREY, WID_DV_CAPTION), SetDataTip(WIDGET_STRING(CAPTION), STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
                NWidget(WWT_SHADEBOX, COLOUR_GREY),
                NWidget(WWT_DEFSIZEBOX, COLOUR_GREY),
                NWidget(WWT_STICKYBOX, COLOUR_GREY),
        EndContainer(),
        NWidget(NWID_HORIZONTAL),
                NWidget(WWT_MATRIX, COLOUR_GREY, WID_DV_LIST), SetMinimalSize(0, 0), SetFill(1, 0), SetResize(1, 1), SetScrollbar(WID_DV_SCROLLBAR),
                NWidget(NWID_VSCROLLBAR, COLOUR_GREY, WID_DV_SCROLLBAR),
        EndContainer(),
        NWidget(NWID_HORIZONTAL),
                NWidgetFunction(ArrivalsButton<Tdeparture_type, Tdepartures_from>),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_VEHICLE), SetMinimalSize(6, 12), SetResize(1, 0), SetFill(1, 1), SetDataTip(STR_DEPARTURES_VEHICLES_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_VEHICLES_TOOLTIP : STR_ARRIVALS_GROUPS_TOOLTIP),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_GROUP), SetMinimalSize(6, 12), SetResize(1, 0), SetFill(1, 1), SetDataTip(STR_DEPARTURES_GROUPS_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_GROUPS_TOOLTIP : STR_ARRIVALS_GROUPS_TOOLTIP),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_COMPANY), SetMinimalSize(6, 12), SetResize(1, 0), SetFill(1, 1), SetDataTip(STR_DEPARTURES_COMPANIES_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_COMPANIES_TOOLTIP : STR_ARRIVALS_COMPANIES_TOOLTIP),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_TRAINS), SetMinimalSize(14, 12), SetFill(0, 1), SetDataTip(STR_DEPARTURES_SHOW_TRAINS_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_SHOW_TRAINS_TOOLTIP : STR_ARRIVALS_SHOW_TRAINS_TOOLTIP),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_BUSES), SetMinimalSize(14, 12), SetFill(0, 1), SetDataTip(STR_DEPARTURES_SHOW_BUSES_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_SHOW_BUSES_TOOLTIP : STR_ARRIVALS_SHOW_BUSES_TOOLTIP),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_LORRIES), SetMinimalSize(14, 12), SetFill(0, 1), SetDataTip(STR_DEPARTURES_SHOW_LORRIES_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_SHOW_LORRIES_TOOLTIP : STR_ARRIVALS_SHOW_LORRIES_TOOLTIP),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_SHIPS), SetMinimalSize(14, 12), SetFill(0, 1), SetDataTip(STR_DEPARTURES_SHOW_SHIPS_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_SHOW_SHIPS_TOOLTIP : STR_ARRIVALS_SHOW_SHIPS_TOOLTIP),
                NWidget(WWT_TEXTBTN, COLOUR_GREY, WID_DV_SHOW_PLANES), SetMinimalSize(14, 12), SetFill(0, 1), SetDataTip(STR_DEPARTURES_SHOW_AIRCRAFT_BUTTON, Tdeparture_type == DT_DEPARTURE ? STR_DEPARTURES_SHOW_AIRCRAFT_TOOLTIP : STR_ARRIVALS_SHOW_AIRCRAFT_TOOLTIP),
                NWidget(WWT_RESIZEBOX, COLOUR_GREY),
        EndContainer(),
};

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
static WindowDesc _departures_desc(
        WDP_AUTO, Tdeparture_type == DT_DEPARTURE ? "view_departures" : "view_arrivals", 249, 117,
        Tdeparture_type == DT_DEPARTURE ? WC_DEPARTURES : WC_ARRIVALS, WC_NONE,
        0,
        _nested_departures_widgets<Tdeparture_type, Tdepartures_from>, lengthof((_nested_departures_widgets<Tdeparture_type, Tdepartures_from>))
);

/**
 * Opens DeparturesWindow for given station
 *
 * @param station station which window should be opened
 */
template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
void ShowDeparturesWindow(StationID station)
{
        AllocateWindowDescFront<DeparturesWindow<Tdeparture_type, Tdepartures_from> >(&_departures_desc<Tdeparture_type, Tdepartures_from>, station);
}

template void ShowDeparturesWindow<DT_DEPARTURE, DF_STATION>(StationID station);
template void ShowDeparturesWindow<DT_DEPARTURE, DF_WAYPOINT>(StationID station);
template void ShowDeparturesWindow<DT_ARRIVAL, DF_STATION>(StationID station);
template void ShowDeparturesWindow<DT_ARRIVAL, DF_WAYPOINT>(StationID station);

static StringID DestinationIcon(VehicleType vehicle_type, StationID station)
{
        StringID icon = STR_EMPTY;
        StationFacility facilities = Station::Get(station)->facilities;
        switch (vehicle_type) {
                case VEH_ROAD:
                        if (facilities & FACIL_TRAIN) {
                                icon = STR_DEPARTURES_STATION_TRAIN;
                        }
                        /* fall through */

                case VEH_TRAIN:
                        if (facilities & FACIL_DOCK) {
                                icon = STR_DEPARTURES_STATION_SHIP;
                        }
                        /* fall through */

                case VEH_SHIP:
                        if (facilities & FACIL_AIRPORT) {
                                icon = STR_DEPARTURES_STATION_PLANE;
                        }
                        break;

                default:
                        break;
        }

        return icon;
}

static char* CallingAtList(const SmallVector<CallingAt, 8> *calling_at, uint begin_index, uint end_index, size_t buf_length)
{
        char *current_buf = new char[buf_length];
        char *next_buf = new char[buf_length];

        SetDParam(0, (*calling_at)[begin_index].station);
        GetString(next_buf, STR_DEPARTURES_CALLING_AT_FIRST_STATION, next_buf + buf_length);

        std::swap(current_buf, next_buf);

        if (end_index - begin_index > 0) {
                for (uint i = begin_index + 1; i < end_index; i++) {
                        SetDParam(0, (uint64)current_buf);
                        SetDParam(1, (*calling_at)[i].station);
                        GetString(next_buf, STR_DEPARTURES_CALLING_AT_STATION, next_buf + buf_length);
                        std::swap(current_buf, next_buf);
                }

                SetDParam(0, (uint64)current_buf);
                SetDParam(1, (*calling_at)[end_index].station);
                GetString(next_buf, STR_DEPARTURES_CALLING_AT_LAST_STATION, next_buf + buf_length);
                std::swap(current_buf, next_buf);
        }

        delete[] next_buf;

        return current_buf;
}

template <DepartureType Tdeparture_type, DeparturesFrom Tdepartures_from>
void DeparturesWindow<Tdeparture_type, Tdepartures_from>::DrawDeparturesList(const Rect &r) const
{
        /* Draw the black background. */
        GfxFillRect(r.left + 1, r.top, r.right - 1, r.bottom, PC_BLACK);

        int left = r.left + WD_MATRIX_LEFT;
        int right = r.right - WD_MATRIX_RIGHT;

        bool rtl = _current_text_dir == TD_RTL;
        bool ltr = !rtl;

        int text_offset = WD_FRAMERECT_RIGHT;
        int text_left  = left  + (rtl ?           0 : text_offset);
        int text_right = right - (rtl ? text_offset :           0);

        int y = r.top + 1;

        if (vehicle_types.none()) {
                DrawString(text_left, text_right, y, STR_DEPARTURES_NONE_SELECTED);
                return;
        }

        if (departures.size() == 0) {
                switch (Tdepartures_from) {
                        case DF_STATION:
                                DrawString(text_left, text_right, y, STR_DEPARTURES_EMPTY);
                                break;

                        case DF_WAYPOINT:
                                DrawString(text_left, text_right, y, STR_DEPARTURES_WAYPOINT_EMPTY);
                                break;
                }
                return;
        }

        /* Work out the length of the buffer that will be needed to construct the calling at list. */
        uint station_length;
        uint comma_length;
        uint and_length;
        uint period_length;
        uint continues_length;

        char scratch[512];
        char empty_str[] = "";

        SetDParam(0, INVALID_STATION);
        GetString(scratch, STR_STATION_NAME, lastof(scratch));
        station_length = strlen(scratch);

        SetDParam(0, (uint64)empty_str);
        SetDParam(1, INVALID_STATION);
        GetString(scratch, STR_DEPARTURES_CALLING_AT_STATION, lastof(scratch));
        comma_length = strlen(scratch) - station_length;

        SetDParam(0, (uint64)empty_str);
        SetDParam(1, INVALID_STATION);
        GetString(scratch, STR_DEPARTURES_CALLING_AT_LAST_STATION, lastof(scratch));
        and_length = strlen(scratch) - station_length;

        SetDParam(0, (uint64)empty_str);
        GetString(scratch, STR_DEPARTURES_CALLING_AT_LIST, lastof(scratch));
        period_length = strlen(scratch);

        SetDParam(0, (uint64)empty_str);
        SetDParam(1, (uint64)empty_str);
        GetString(scratch, STR_DEPARTURES_CALLING_AT_CONTINUES, lastof(scratch));
        continues_length = strlen(scratch);

        for (typename PrintableDepartureList<Tdeparture_type>::const_iterator it = departures.begin(); it != departures.end(); it++, y += entry_height) {
                text_left  = left  + (rtl ?           0 : text_offset);
                text_right = right - (rtl ? text_offset :           0);

                /* Date */
                if (show_scheduled_arrivals) {
                        SetDParam(0, it->scheduled_arrival);
                        SetDParam(1, it->scheduled_departure);
                        if (ltr) {
                                DrawString(text_left, text_left + date_width, y, STR_DEPARTURES_ARRIVAL_AND_DEPARTURE_DATES);
                                text_left += date_width + 3;
                        } else {
                                DrawString(text_right - date_width, text_right, y, STR_DEPARTURES_ARRIVAL_AND_DEPARTURE_DATES);
                                text_right -= (date_width + 3);
                        }
                } else {
                        switch (Tdeparture_type) {
                                case DT_DEPARTURE:
                                        SetDParam(0, it->scheduled_departure);
                                        break;

                                case DT_ARRIVAL:
                                        SetDParam(0, it->scheduled_arrival);
                                        break;
                        }
                        if (ltr) {
                                DrawString(text_left, text_left + date_width, y, STR_DEPARTURES_DEPARTURE_DATE);
                                text_left += date_width + 3;
                        } else {
                                DrawString(text_right - date_width, text_right, y, STR_DEPARTURES_DEPARTURE_DATE);
                                text_right -= (date_width + 3);
                        }
                }

                /* Icon */
                StringID icon_string = INVALID_STRING_ID;
                switch (it->vehicle_type) {
                        case VEH_TRAIN:
                                icon_string = STR_DEPARTURES_ICON_TRAIN;
                                break;

                        case VEH_ROAD:
                                if (IsCargoInClass(it->vehicle->cargo_type, CC_PASSENGERS)) {
                                        icon_string = STR_DEPARTURES_ICON_BUS;
                                } else {
                                        icon_string = STR_DEPARTURES_ICON_LORRY;
                                }
                                break;

                        case VEH_SHIP:
                                icon_string = STR_DEPARTURES_ICON_SHIP;
                                break;

                        case VEH_AIRCRAFT:
                                icon_string = STR_DEPARTURES_ICON_PLANE;
                                break;

                        default:
                                break;
                }
                if (ltr) {
                        DrawString(text_left, text_left + icon_width, y, icon_string);
                        text_left += icon_width + 3;
                } else {
                        DrawString(text_right - icon_width, text_right, y, icon_string);
                        text_right -= (icon_width + 3);
                }

                /* Company */
                if (labels[SL_COMPANIES]) {
                        SetDParam(0, it->vehicle->owner);
                        if (ltr) {
                                DrawString(text_right - company_width, text_right, y + 1, STR_DEPARTURES_COMPANY);
                                text_right -= (company_width + 3);
                        } else {
                                DrawString(text_left, text_left + company_width, y + 1, STR_DEPARTURES_COMPANY);
                                text_left += company_width + 3;
                        }
                }

                /* Group */
                if (labels[SL_GROUPS]) {
                        GroupID group = it->vehicle->group_id;
                        if (group != DEFAULT_GROUP && group != INVALID_GROUP) {
                                SetDParam(0, group);
                                if (ltr) {
                                        DrawString(text_right - group_width, text_right, y + 1, STR_DEPARTURES_GROUP);
                                        text_right -= (group_width + 3);
                                } else {
                                        DrawString(text_left, text_left + group_width, y + 1, STR_DEPARTURES_GROUP);
                                        text_left += group_width + 3;
                                }
                        } else {
                                if (ltr) {
                                        text_right -= (group_width + 3);
                                } else {
                                        text_left += group_width + 3;
                                }
                        }
                }

                /* Vehicle */
                if (labels[SL_VEHICLES]) {
                        SetDParam(0, it->vehicle->index);
                        if (ltr) {
                                DrawString(text_right - vehicle_width, text_right, y + 1, STR_DEPARTURES_VEHICLE);
                                text_right -= (vehicle_width + 3);
                        } else {
                                DrawString(text_left, text_left + vehicle_width, y + 1, STR_DEPARTURES_VEHICLE);
                                text_left += vehicle_width + 3;
                        }
                }

                /* Status */
                StringID status_string = INVALID_STRING_ID;
                switch (it->status) {
                        case DS_ON_TIME:
                                status_string = STR_DEPARTURES_ON_TIME;
                                break;

                        case DS_ARRIVED:
                                status_string = STR_DEPARTURES_ARRIVED;
                                break;

                        case DS_CANCELLED:
                                status_string = STR_DEPARTURES_CANCELLED;
                                break;

                        case DS_DELAYED:
                                status_string = STR_DEPARTURES_DELAYED;
                                break;

                        case DS_EXPECTED:
                                status_string = STR_DEPARTURES_EXPECTED;
                                SetDParam(0, it->expected);
                                break;
                }
                if (ltr) {
                        DrawString(text_right - status_width, text_right, y, status_string);
                        text_right -= (status_width + 3);
                } else {
                        DrawString(text_left, text_left + status_width, y, status_string);
                        text_left += (status_width + 3);
                }

                /* Destination */
                StationID destination = (*(it->calling_at))[it->destination_index].station;
                StringID destination_icon = DestinationIcon(it->vehicle_type, destination);

                if (it->via == INVALID_STATION || it->via == destination) {
                        SetDParam(0, destination);
                        SetDParam(1, destination_icon);
                        DrawString(text_left, text_right, y, STR_DEPARTURES_DESTINATION);
                } else {
                        StringID via_icon = DestinationIcon(it->vehicle_type, it->via);

                        SetDParam(0, destination);
                        SetDParam(1, destination_icon);
                        SetDParam(2, it->via);
                        SetDParam(3, via_icon);
                        int width = GetStringBoundingBox(STR_DEPARTURES_DESTINATION_VIA_STATION).width;
                        if (width <= text_right - text_left) {
                                SetDParam(0, destination);
                                SetDParam(1, destination_icon);
                                SetDParam(2, it->via);
                                SetDParam(3, via_icon);
                                DrawString(text_left, text_right, y, STR_DEPARTURES_DESTINATION_VIA_STATION);
                        } else if (tick_count % (DAY_TICKS * 2) < DAY_TICKS) {
                                SetDParam(0, destination);
                                SetDParam(1, destination_icon);
                                DrawString(text_left, text_right, y, STR_DEPARTURES_DESTINATION_VIA);
                        } else {
                                SetDParam(0, it->via);
                                SetDParam(1, via_icon);
                                DrawString(text_left, text_right, y, STR_DEPARTURES_VIA_STATION);
                        }
                }

                /* Reset left and right for the second line. */
                text_left  = left  + (rtl ?           0 : text_offset);
                text_right = right - (rtl ? text_offset :           0);

                int bottom_y = y + FONT_HEIGHT_NORMAL;
                if (_settings_client.gui.larger_departures_font) {
                        bottom_y += 1;
                }

                /* Calling at */
                if (ltr) {
                        DrawString(text_left, text_right, bottom_y, _settings_client.gui.larger_departures_font ? STR_DEPARTURES_CALLING_AT_LARGE : STR_DEPARTURES_CALLING_AT);
                        text_left += calling_at_width + 2;
                } else {
                        DrawString(text_left, text_right, bottom_y, _settings_client.gui.larger_departures_font ? STR_DEPARTURES_CALLING_AT_LARGE : STR_DEPARTURES_CALLING_AT);
                        text_right -= (calling_at_width + 2);
                }

                /* Station list */

                /* Include a space for the null terminator. */
                uint length = comma_length * (it->calling_at->Length() - 1) + (and_length * 2) + period_length + continues_length + 1;
                for (const CallingAt *calling_at = it->calling_at->Begin(); calling_at != it->calling_at->End(); calling_at++) {
                        SetDParam(0, calling_at->station);
                        GetString(scratch, STR_STATION_NAME, lastof(scratch));
                        length += strlen(scratch);
                }

                uint num_stations = it->calling_at->Length();

                size_t stations_in_first_segment;
                switch (Tdeparture_type) {
                        case DT_DEPARTURE:
                                stations_in_first_segment = it->destination_index + 1;
                                break;

                        case DT_ARRIVAL:
                                stations_in_first_segment = num_stations;
                                break;
                }

                char *first_segment_buf = CallingAtList(it->calling_at, 0, stations_in_first_segment - 1, length);

                char *calling_at_buf;
                if (stations_in_first_segment < num_stations) {
                        char *second_segment_buf = CallingAtList(it->calling_at, stations_in_first_segment, num_stations - 1, length);
                        calling_at_buf = new char[length];

                        SetDParam(0, (uint64)first_segment_buf);
                        SetDParam(1, (uint64)second_segment_buf);
                        GetString(calling_at_buf, STR_DEPARTURES_CALLING_AT_CONTINUES, calling_at_buf + length);

                        delete[] first_segment_buf;
                        delete[] second_segment_buf;
                } else {
                        calling_at_buf = first_segment_buf;
                }

                char *calling_at_buf_formatted = new char[length];

                SetDParam(0, (uint64)calling_at_buf);
                GetString(calling_at_buf_formatted, _settings_client.gui.larger_departures_font ? STR_DEPARTURES_CALLING_AT_LIST_LARGE : STR_DEPARTURES_CALLING_AT_LIST, calling_at_buf_formatted + length);

                int list_width = GetStringBoundingBox(calling_at_buf_formatted, CallingAtFontSize()).width + 4;

                if (list_width < text_right - text_left) {
                        DrawString(text_left, text_right, bottom_y, calling_at_buf_formatted);
                } else {
                        DrawPixelInfo tmp_dpi;

                        if (!FillDrawPixelInfo(&tmp_dpi, text_left, bottom_y, text_right - text_left, CallingAtFontHeight() + 3)) {
                                continue;
                        }

                        DrawPixelInfo *old_dpi = _cur_dpi;
                        _cur_dpi = &tmp_dpi;

                        /* The scrolling text starts out of view at the right of the screen and finishes when it is out of view at the left of the screen. */
                        int total_scroll_width = list_width + text_right - text_left + calling_at_width;
                        int pos;
                        if (ltr) {
                                pos = text_right - (this->tick_count % total_scroll_width);
                        } else {
                                pos = text_left + (this->tick_count % total_scroll_width);
                        }

                        if (ltr) {
                                DrawString(pos, INT16_MAX, 0, calling_at_buf_formatted, TC_FROMSTRING,  SA_LEFT | SA_FORCE);
                        } else {
                                DrawString(-INT16_MAX, pos, 0, calling_at_buf_formatted, TC_FROMSTRING, SA_RIGHT | SA_FORCE);
                        }

                        _cur_dpi = old_dpi;
                }

                delete[] calling_at_buf;
                delete[] calling_at_buf_formatted;
        }
}
