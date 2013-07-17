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
        PingMonitor( HostAndPort& _hp , HostAndPort& _self , bool _on , int _interval , string _collectionPrefix , string _networkType ){
	    initializeCharts();
    	    self = _self;
	    target = _hp;
	    on = _on;
	    interval = _interval;
	    collectionPrefix = _collectionPrefix; 
	    networkType = _networkType;
	}

        virtual ~PingMonitor(){}
        virtual string name() const { return "PingMonitor"; }

	BSONObj getInfo();
	string getNetworkType();
	string getCollectionPrefix();
	int getInterval(); 
	bool setInterval( int nsecs );
	bool isOn();
	bool turnOn();
	bool turnOff();
	void clearHistory();
 
	BSONObj getMonitorInfo();

	BSONObj calculateStats();
	BSONObj calculateDeltas();

    private:

	boost::mutex _mutex;
	static map< string , string > ERRCODES;
   	static map<string,string> initializeErrcodes();
	static const double socketTimeout = 30.0;
	static BSONObj reqConnChart;
	static BSONObj recConnChart;

	HostAndPort self;
	HostAndPort target;
	bool on;
	int interval;
	string collectionPrefix;		
    	string networkType;

    	virtual void run();
	void doPingForTarget(); //redirects to doPingForCluster() or doPingForReplset()
	void doPingForReplset();
	void doPingForCluster();

	void addNewNodes( HostAndPort& target , BSONObj& nodes );

	static void initializeCharts();
	void addError(const string& key , const string& err , map<string, vector<string> >& errors);
	void addWarning(const string& key , const string& warning , map<string, vector<string> >& warnings);

	int getShardServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	int getMongosServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	int getConfigServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );

	void buildGraph( BSONObj& nodes , BSONObjBuilder& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );
	void buildIdMap( BSONObj& nodes , BSONObjBuilder& idMap );
	void diagnose( BSONObj& nodes , BSONObj& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings );

	bool isReqConn( const string& src , const string& tgt);
	bool isRecConn( const string& src , const string& tgt);

    };

    BSONObj convertToBSON( map< string , vector<string> >& m ); 
}
