 // ping_monitor_thread_manager.h

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
#include "mongo/util/net/hostandport.h"
#include "boost/thread/mutex.hpp"
#include "boost/thread/thread.hpp"
#include "mongo/client/dbclientinterface.h"

 namespace mongo {
    
    class PingMonitorThreadManager {

    public:
        PingMonitorThreadManager(){}
        virtual ~PingMonitorThreadManager(){}
        virtual string name() const { return "PingMonitorThreadManager"; }

	//for all targets

	static BSONObj getAllTargets();
	static BSONObj getAllTargetsWithInfo();
	static void clearAllHistory();

	//for a specific target

	// if self is not set, determine own HostAndPort
	// calls PingMonitor() constructor
	static BSONObj createTarget( HostAndPort& hp , bool on=true , int interval=15 , string collectionPrefix="" );

	// accessor methods

 	static bool hasTarget( HostAndPort& hp );
	static bool isOn( HostAndPort& hp );
	static BSONObj getInfo( HostAndPort& hp );
	static BSONObj getNetworkType( HostAndPort& hp );
	static BSONObj getCollectionPrefix( HostAndPort& hp );
	static int getInterval( HostAndPort& hp ); 
	static BSONObj getMonitorResults( HostAndPort& hp );

	// setter methods

	static bool setInterval( HostAndPort& hp , int nsecs );
	static bool turnOn( HostAndPort& hp );
	static bool turnOff( HostAndPort& hp );
	static void clearHistory( HostAndPort& hp );

    private:

	static map< HostAndPort , PingMonitor > targets;
	bool selfSet;
	HostAndPort self;

	static bool canConnect( HostAndPort& hp );
	static BSONObj determineNetworkType( HostAndPort& hp );
	//TODO:
	static HostAndPort findSelf();
    };

}
