/**
 ******************************************************************************
 *
 * @file       GCSControlgadgetfactory.cpp
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @addtogroup GCSPlugins GCS Plugins
 * @{
 * @addtogroup GCSControlGadgetPlugin GCSControl Gadget Plugin
 * @{
 * @brief A gadget to control the UAV, either from the keyboard or a joystick
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
#include "magicwaypointgadgetfactory.h"
#include "magicwaypointgadgetwidget.h"
#include "magicwaypointgadget.h"
#include <coreplugin/iuavgadget.h>
#include <QDebug>

MagicWaypointGadgetFactory::MagicWaypointGadgetFactory(QObject *parent)
    : IUAVGadgetFactory(QString("MagicWaypointGadget"), tr("Magic Waypoint"), parent)
{
}

MagicWaypointGadgetFactory::~MagicWaypointGadgetFactory()
{
}

IUAVGadget *MagicWaypointGadgetFactory::createGadget(QWidget *parent)
{
    MagicWaypointGadgetWidget *gadgetWidget = new MagicWaypointGadgetWidget(parent);
    return new MagicWaypointGadget(QString("MagicWaypointGadget"), gadgetWidget, parent);
}
