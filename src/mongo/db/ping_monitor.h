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

	static map< string , string > ERRCODES;

	static BSONObj getMonitorResults();

	static HostAndPort getTarget();
	static string getTargetNetworkType();
	static BSONObj setTarget( HostAndPort hp );
	static bool getTargetIsSet();

	static bool getIsMonitoring();
	static bool turnOffMonitoring();
	static bool turnOnMonitoring();
	static bool switchMonitoringTarget( HostAndPort hp );
	static void clearMonitoringHistory();
   
	static bool setPingInterval( int nsecs );
	static int getPingInterval(); 
    private:

	static boost::mutex _mutex;

	static HostAndPort target;
	static string targetNetworkType;
	static bool targetIsSet; 
	static int pingInterval;
	
//	static BSONObj monitorResults; // this will eventually be replaced by local db

	static bool isMonitoring;
	
	static BSONObj canConnect( HostAndPort hp );
	static bool determineNetworkType( DBClientConnection& conn );

	virtual void run();
	static void doPingForTarget(); //redirects to doPingForCluster() or doPingForReplset()
	static void doPingForCluster( DBClientConnection& conn );
	static void doPingForReplset( DBClientConnection& conn );

	static BSONObj reqConnChart;
	static BSONObj recConnChart;

	static map<string,string> initializeErrcodes();
	static void initializeCharts();
	static void addError(const string& key , const string& err , map<string, vector<string> >& errors);
	static void addWarning(const string& key , const string& warning , map<string, vector<string> >& warnings);
	static int getShardServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	static int getMongosServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	static int getConfigServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	static void buildGraph( BSONObj& nodes , BSONObjBuilder& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	static void buildIdMap( BSONObj& nodes , BSONObjBuilder& idMap );
	static void diagnose( BSONObj& nodes , BSONObj& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	static bool isReqConn( const string& src , const string& tgt);
	static bool isRecConn( const string& src , const string& tgt);
	friend void startPingBackgroundJob();
    };

    void startPingBackgroundJob();
    
    BSONObj convertToBSON( map< string , vector<string> >& m ); 
}
