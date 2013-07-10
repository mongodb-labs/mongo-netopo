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
#include "mongo/client/dbclientinterface.h"

 namespace mongo {
    
    class PingMonitor : public BackgroundJob {

    public:
        PingMonitor(){}
        virtual ~PingMonitor(){}
        virtual string name() const { return "PingMonitor"; }

	static void setTarget( HostAndPort newTarget );

	static BSONObj getMonitorResults();
	static string getTarget();
	
    private:

	static void doPingForTarget();
	static boost::mutex _mutex;
	static BSONObj monitorResults;
	static HostAndPort target; 
	virtual void run();

	static int getShardServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings );
	static int getMongosServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings );
	static int getConfigServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings );

	static void buildGraph( BSONObj& nodes , BSONObjBuilder& edges , BSONObjBuilder& errors , BSONObjBuilder& warnings );
	
	static void buildIdMap( BSONObj& nodes , BSONObjBuilder& idMap );

	static void diagnose( BSONObj& nodes , BSONObj& edges , BSONObjBuilder& errors , BSONObjBuilder& warnings );

    };

 
    void startPingBackgroundJob();





}
