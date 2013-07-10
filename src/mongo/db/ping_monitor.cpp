// ping_monitor.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "pch.h"

#include "ping_monitor.h"

#include "mongo/base/counter.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/databaseholder.h"
#include "mongo/db/instance.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/util/background.h"


namespace mongo {

    // PingMonitor

    boost::mutex PingMonitor::_mutex;

    BSONObj PingMonitor::monitorResults;
    BSONObj PingMonitor::getMonitorResults(){ return monitorResults; }
 
    string defaultTarget = "localhost:27017";

    HostAndPort PingMonitor::target(defaultTarget);

    void PingMonitor::setTarget( HostAndPort newTarget ){
	target = newTarget;
	// run one ping right now with the updated target so the user gets results from the
	// expected target; otherwise would be more likely that the last pinged target would
	// still be stored in monitorResults and those outdated results would be returned
	// to the user
	doPingForTarget();	
    }

    string PingMonitor::getTarget(){ return target.toString(true); } 

    void PingMonitor::doPingForTarget(){
	BSONObjBuilder resultBuilder;
	resultBuilder.append( "target" , target.toString(true) );
	const string adminDB = "admin";
	string connInfo;
	DBClientConnection conn;
	bool isConnected;
	try{ isConnected = conn.connect( target.toString(true) , connInfo );}
	catch( SocketException& e){ 
	    resultBuilder.append("errmsg" , connInfo);
	    monitorResults = resultBuilder.obj();
	    return; 
	} 
	if( isConnected == false ){
	    resultBuilder.append("errmsg" , connInfo);
	    monitorResults = resultBuilder.obj();
	    return; 
	}

	BSONObjBuilder nodesBuilder, edgesBuilder, idMapBuilder, errorsBuilder, warningsBuilder;
	BSONObj nodes, edges, idMap, errors, warnings;
	int index = 0;

	index = getShardServers( conn , nodesBuilder , index , errorsBuilder , warningsBuilder );
	index = getMongosServers( conn , nodesBuilder , index , errorsBuilder , warningsBuilder );
	index = getConfigServers( conn , nodesBuilder , index , errorsBuilder , warningsBuilder );
	nodes = nodesBuilder.obj();

	buildGraph( nodes , edgesBuilder , errorsBuilder , warningsBuilder );
	edges = edgesBuilder.obj();

	buildIdMap( nodes , idMapBuilder );
	idMap = idMapBuilder.obj();

	diagnose( nodes , edges , errorsBuilder , warningsBuilder );
	errors = errorsBuilder.obj();
	warnings = warningsBuilder.obj();

	resultBuilder.append("nodes" , nodes);   
	resultBuilder.append("edges" , edges);
	resultBuilder.append("idMap" , idMap);
	resultBuilder.append("errors" , errors);
	resultBuilder.append("warnings" , warnings); 

	monitorResults = resultBuilder.obj();	  

    }

    int PingMonitor::getShardServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings ){

	return ++index;
    }
    int PingMonitor::getMongosServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings ){

	return ++index;
    }
    int PingMonitor::getConfigServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings ){

	return ++index;
    }

    void PingMonitor::buildGraph( BSONObj& nodes , BSONObjBuilder& edges , BSONObjBuilder& errors , BSONObjBuilder& warnings ){

    }

    void PingMonitor::buildIdMap( BSONObj& nodes , BSONObjBuilder& idMap ){

    }

    void PingMonitor::diagnose( BSONObj& nodes , BSONObj& edges , BSONObjBuilder& errors , BSONObjBuilder& warnings ){

    }

    void PingMonitor::run() {
//	Client::initThread( name().c_str() );
   
	while ( ! inShutdown() ) {
	    sleepsecs( 2 );
	    LOG(3) << "PingMonitor thread awake" << endl;
	    if( false /*lockedForWriting()*/ ) {
		// note: this is not perfect as you can go into fsync+lock between
		// this and actually doing the delete later
		LOG(3) << " locked for writing" << endl;
		continue;
	    }
	    // if part of a replSet but not in a readable state ( e.g. during initial sync), skip
	    //if ( theReplSet && !theReplSet->state().readable() )
	    //    continue;
	    //TODO: change this to defaulting to self; also to being settable
	    doPingForTarget();
	}
    }

    void startPingBackgroundJob() {
	PingMonitor* pmt = new PingMonitor();
	pmt->go();
    }

} 
