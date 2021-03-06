/**
 ******************************************************************************
 *
 * @file       opmap_zoom_slider_widget.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
 * @addtogroup GCSPlugins GCS Plugins
 *
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup OPMapPlugin Tau Labs Map Plugin
 * @{
 * @brief Tau Labs map plugin
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

#include "opmap_zoom_slider_widget.h"
#include "ui_opmap_zoom_slider_widget.h"

opmap_zoom_slider_widget::opmap_zoom_slider_widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::opmap_zoom_slider_widget)
{
    ui->setupUi(this);
}

opmap_zoom_slider_widget::~opmap_zoom_slider_widget()
{
    delete ui;
}
