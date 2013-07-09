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
#include "mongo/util/net/hostandport.h"
#include "boost/thread/mutex.hpp"
#include "boost/thread/thread.hpp"


 namespace mongo {
    
    class PingMonitor : public BackgroundJob {

    public:
        PingMonitor(){}
        virtual ~PingMonitor(){}
        virtual string name() const { return "PingMonitor"; }

	static void setTarget( HostAndPort newTarget );

	static BSONObj getMonitorResults();
	static string getTarget();

	static void doPingForTarget();
	
    private:

	static boost::mutex _mutex;
	static BSONObj monitorResults;
	static HostAndPort target; 
	virtual void run();



    };

 
    void startPingBackgroundJob();

}
