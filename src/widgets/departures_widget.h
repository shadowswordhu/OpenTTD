
/* $Id$ */

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file departures_widget.h Types related to the departures widgets. */

#ifndef WIDGETS_DEPARTURES_WIDGET_H
#define WIDGETS_DEPARTURES_WIDGET_H

/** Widgets of the #DeparturesWindow class. */
enum DeparturesWidgets {
	WID_DV_CAPTION,            ///< Caption of the window.
	WID_DV_LIST,               ///< Departures list.
	WID_DV_SCROLLBAR,          ///< Scrollbar.
	WID_DV_SHOW_ARRIVAL_TIME,  ///< Arrival times button.
	WID_DV_SHOW_TRAINS,        ///< Trains button.
	WID_DV_SHOW_BUSES,         ///< Buses button.
	WID_DV_SHOW_LORRIES,       ///< Lorries button.
	WID_DV_SHOW_SHIPS,         ///< Ships button.
	WID_DV_SHOW_PLANES,        ///< Planes button.
	WID_DV_SHOW_VEHICLE,       ///< Vehicle names button.
	WID_DV_SHOW_GROUP,         ///< Vehicle groups button.
	WID_DV_SHOW_COMPANY,       ///< Companies button.
};

#endif /* WIDGETS_DEPARTURES_WIDGET_H */
