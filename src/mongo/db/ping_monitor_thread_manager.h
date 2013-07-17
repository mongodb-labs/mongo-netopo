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
	static BSONObj getInfo();
	
	//for a specific target

	// if self is not set, determine own HostAndPort
	// calls PingMonitor() constructor
	static BSONObj createTarget( HostAndPort& , bool on/*=true*/ , int interval/*=15*/ , string collectionPrefix/*=""*/ );

	// accessor methods

 	static bool hasTarget( HostAndPort& );
	static bool isOn( HostAndPort& );
	static BSONObj getTargetInfo( HostAndPort& );
	static BSONObj getNetworkType( HostAndPort& );
	static BSONObj getCollectionPrefix( HostAndPort& );
	static int getInterval( HostAndPort& ); 
	static BSONObj getMonitorResults( HostAndPort& );

	// setter methods

	static bool setInterval( HostAndPort& , int );
	static bool turnOn( HostAndPort& );
	static bool turnOff( HostAndPort& );
	static void clearHistory( HostAndPort& );

    private:

	static map< HostAndPort , PingMonitor > targets;
	
	static HostAndPort self;
	static bool selfSet;

	static HostAndPort findSelf(); 

	static const double socketTimeout = 30.0;

	static BSONObj canConnect( HostAndPort& hp );
	static BSONObj determineNetworkType( HostAndPort& hp );

   };

}
