 // ping_monitor.h

 /**
 *    Copyright (C) 20080gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "pch.h"

#include "ping_monitor.h"
#include "mongo/util/background.h"

 namespace mongo {
       class PingMonitor : public BackgroundJob {
    public:
        PingMonitor(){}
        virtual ~PingMonitor(){}
        virtual string name() const { return "PingMonitor"; }
        static int getNumTimes();
	void doPingForHost( const string& hp );
    private:
        static int numTimes;
        virtual void run();
    };
 
    void startPingBackgroundJob();

}
