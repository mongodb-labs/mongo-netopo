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
#include "mongo/db/instance.h"
#include "mongo/db/cmdline.h"

 namespace mongo {
    
    class PingMonitor : public BackgroundJob {

    friend class PingMonitorThreadManager;

    public:
        PingMonitor( HostAndPort& _hp , bool _on , int _interval , string _collectionPrefix , string _networkType ){
	    initializeCharts();
	    target = _hp;
	    on = _on;
	    interval = _interval;
	    collectionPrefix = _collectionPrefix; 
	    networkType = _networkType;
	    numPings = 0;

	    //TODO: find own HostAndPort more cleanly?
	    string selfHostName = getHostName();
	    int selfPort = cmdLine.port;
	    stringstream ss;
	    ss << selfPort;
	    self = HostAndPort( selfHostName + ":" + ss.str() );

	    //TODO: choose db based on type of mongo instance
	    db = "test";
	    graphsLocation = db+"."+outerCollection+"."+collectionPrefix+"."+graphs;
	    statsLocation = db+"."+outerCollection+"."+collectionPrefix+"."+stats;
	    deltasLocation = db+"."+outerCollection+"."+collectionPrefix+"."+deltas;
	    allNodesLocation = db+"."+outerCollection+"."+collectionPrefix+"."+allNodes;


/*	    BSONObj isMasterResults;
	    dbc.runCommand( "admin" , BSON("isMaster"<<1) , isMasterResults );
            if( isMasterResults["msg"].trueValue() )
                db = "config";
            if( isMasterResults["setName"].trueValue() )
                db = "local";
*/

//	    cout << "my HostAndPort : " << HostAndPort::me().toString() << endl;
	    alive = true;
	}

        virtual ~PingMonitor(){}
        virtual string name() const { return "PingMonitor"; }

	BSONObj getInfo();
	string getNetworkType();
	string getCollectionPrefix();
	int getInterval(); 
	int getNumPings();
	bool setInterval( int nsecs );
	bool isOn();
	bool setOn( bool _on );
	void clearHistory();
	void shutdown();
 
	BSONObj getMonitorInfo();

	void calculateStats();
	void calculateDeltas();

    private:

	//DBDirectClient dbc;
	boost::mutex _mutex;
	static map< string , string > ERRCODES;
   	static map<string,string> initializeErrcodes();
	static const double socketTimeout = 30.0;
	static BSONObj reqConnChart;
	static BSONObj recConnChart;

	HostAndPort target;
	HostAndPort self;
	bool on;
	bool alive;
	int interval;
	string collectionPrefix;		
    	string networkType;
	int numPings;
	long long lastPingNetworkMillis;
	string graphsLocation;
	string statsLocation;
	string deltasLocation;
	string allNodesLocation;
 
	// data stored in DBDirectClient's
	// [local|config].pingMonitor.[clusterId|replsetName].[graphs|stats|deltas]
	// local if this is a mongod, config if mongos
	// clusterId if target is shardedCluster, replsetName if target is replicaSet
	// graphs, stats, and deltas stored for both types
	string db;
	static const string outerCollection;
	static const string graphs;
	static const string deltas;
	static const string stats;
	static const string allNodes;
	
    	virtual void run();
	void doPingForTarget(); //redirects to doPingForCluster() or doPingForReplset()


	bool writeMonitorData( BSONObj& toWrite );

	void doPingForReplset();

	void getSetServers( HostAndPort& target , BSONObjBuilder& nodesBuilder , map< string , vector<string> >& errorsBuilder , map< string, vector<string> >& warningsBuilder );



	void doPingForCluster();

	void addNewNodes( BSONObj& nodes );

	static void initializeCharts();
	void addError(const string& key , const string& err , map<string, vector<string> >& errors);
	void addWarning(const string& key , const string& warning , map<string, vector<string> >& warnings);

	int getShardServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	int getMongosServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	int getConfigServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );

	void buildGraph( BSONObj& nodes , BSONObjBuilder& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	void buildIdMap( BSONObj& nodes , BSONObjBuilder& idMap );
	void diagnose( BSONObj& nodes , BSONObj& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );


    void collectClientInfoHelper( HostAndPort& hp , BSONObjBuilder& newHost , const string& db , string cmdName );
    void collectClientInfo( string connString , BSONObjBuilder& newHost , BSONObj& type , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );

	bool isReqConn( const string& src , const string& tgt);
	bool isRecConn( const string& src , const string& tgt);

    };

    BSONObj convertToBSON( map< string , vector<string> >& m ); 
}
