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
#include "mongo/db/repl/is_master.h"
#include "mongo/util/background.h"
#include "mongo/client/connpool.h"
#include <boost/tokenizer.hpp>
#include <unistd.h>
#include <sstream>
#include "boost/date_time/posix_time/posix_time.hpp"

#include "mongo/util/net/sock.h";

namespace mongo {

    // PingMonitor

    BSONObj PingMonitor::reqConnChart;
    BSONObj PingMonitor::recConnChart;
    const string PingMonitor::outerCollection = "pingMonitor";
    const string PingMonitor::graphs = "graphs";
    const string PingMonitor::deltas = "deltas";
    const string PingMonitor::stats = "stats";

    // Error codes
    map< string , string > PingMonitor::ERRCODES = PingMonitor::initializeErrcodes();
    map< string , string > PingMonitor::initializeErrcodes(){
	map<string,string> m;
	m["MISSING_REQ_CONN"] = "Missing required connection to ";
	m["MISSING_REC_CONN"] = "Missing recommended connection to ";
	m["TARGET_NOT_NETWORK_MASTER"] = "Target is not the master of a network (primary of replica set, mongos instance, or master in master-slave relationship).";
	m["NOT_ENOUGH_RECORDS"] = "Not enough PingMonitor records to complete this action.";
	m["CAN'T_MAKE_CLIENT_CONNECTION"] = "Cannot make client connection to host.";
	return m;
    };

    // Retrieval commands 

    //TODO , call other get functions from within
    BSONObj PingMonitor::getInfo(){
	BSONObjBuilder toReturn;
	toReturn.append( "networkType" , networkType );
	toReturn.append( "intervalSecs" , interval );
	toReturn.append( "collectionPrefix" , collectionPrefix );
	toReturn.append( "on" , on );
	toReturn.append( "numPingsDone" , numPings );
	toReturn.append( "lastPingNetworkMillis" , lastPingNetworkMillis );
	return toReturn.obj();
    }

    string PingMonitor::getNetworkType(){
	return networkType;
    }

    string PingMonitor::getCollectionPrefix(){
 	return collectionPrefix;
    }

    int PingMonitor::getInterval(){
	return interval; 
    }

    int PingMonitor::getNumPings(){
	return numPings;
    }
 
    bool PingMonitor::setInterval( int nsecs ){ 
	interval = nsecs;
        return true; 
    }

    bool PingMonitor::isOn(){
	return on;
    } 

    // starts/stops monitoring process but does not reset target or clear monitoring history
    // thread continues to spin in the background
    bool PingMonitor::setOn( bool _on ){
	on = _on;
	return true;
    } 

    //TODO
    void PingMonitor::clearHistory(){

    }

   
    // Retrieve from ping monitor storage database
    BSONObj PingMonitor::getMonitorInfo(){ 
	BSONObjBuilder show;
	BSONObj latestMonitorInfo;
	scoped_ptr<ScopedDbConnection> connPtr;
	auto_ptr<DBClientCursor> cursor;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() , socketTimeout ) ); //TODO: save self
	    ScopedDbConnection& conn = *connPtr;
	    // sort snapshots in reverse chronological order
	    // to ensure the first returned is the lastest
	    scoped_ptr<DBClientCursor> cursor( conn->query( "test.pingmonitor" , Query(BSONObj()).sort("_id",-1)) ) ;
	    latestMonitorInfo = cursor->next();
	    connPtr->done(); 
	}
	catch( DBException& e ){
	    show.append("errmsg" , e.toString() );
	    return show.obj();	    	
	}
	BSONObjBuilder nodeShow;
	BSONObjBuilder edgeShow;
	for (BSONObj::iterator i = latestMonitorInfo.getObjectField("nodes").begin(); i.more(); ){
	    BSONElement nodeElem = i.next();
	    BSONObj nodeObj = nodeElem.embeddedObject(); 
	    BSONObjBuilder nodeData;
	    nodeData.append( "hostName" , nodeObj["hostName"].valuestrsafe() );
	    nodeData.append( "process" , nodeObj["process"].valuestrsafe() );
	    nodeData.append( "role" , (nodeObj.getObjectField("type"))["role"].valuestrsafe() );
	    nodeData.append( "uptimeMillis" , nodeObj.getObjectField("serverStatus")["uptimeMillis"]._numberLong() );
//	    nodeData.append( "key" , nodeObj["key"].valuestrsafe() );
//	    nodeData.append( "serverStatus" , nodeObj.getObjectField("serverStatus").toString() );
	    nodeShow.append( nodeElem.fieldName() , nodeData.obj() ); 
	}
	for (BSONObj::iterator i = latestMonitorInfo.getObjectField("edges").begin(); i.more(); ){
	    const BSONElement srcElem = i.next();
	    const BSONObj srcObj = srcElem.embeddedObject();
	    for( BSONObj::iterator j = srcObj.begin(); j.more(); ){
		const BSONElement tgtElem = j.next();
		const BSONObj tgtObj = tgtElem.embeddedObject();
		string edgestr = "";
		edgestr += srcElem.fieldName();
		edgestr += " -> ";
		edgestr += tgtElem.fieldName();
		edgestr += tgtObj["pingTimeMicrosecs"].valuestrsafe();
		edgeShow.append( edgestr , tgtObj["isConnected"].boolean() );
	    }
	}
//    	show.append( "edges_full" , latestMonitorInfo.getObjectField("edges") ); 
//	show.append( "nodes_full" , latestMonitorInfo.getObjectField("nodes") );
	show.append( "edges" , edgeShow.obj() );
	show.append( "nodes" , nodeShow.obj() );
	show.append( "errors" , latestMonitorInfo.getObjectField("errors") );
	show.append( "warnings" , latestMonitorInfo.getObjectField("warnings") );
	return show.obj(); 
    }
    

    void PingMonitor::doPingForTarget(){

	if( on == false )
	    return;

	if( networkType == "replicaSet" )
	    doPingForReplset();
	else if( networkType == "shardedCluster" )
	    doPingForCluster();
    }

    void PingMonitor::doPingForReplset(){

	// create a typedef for the Graph type
	// Add nodes to the graph
	// Write out the edges in the graph
	// declare a graph object
	// add the edges to the graph object

	BSONObjBuilder resultBuilder;
	resultBuilder.append( "target" , target.toString(true) );

	BSONObjBuilder nodesBuilder, edgesBuilder, idMapBuilder;
	BSONObj nodes, edges, idMap, errors, warnings;
	map<string, vector<string> > errorsBuilder, warningsBuilder;

	getSetServers( target , nodesBuilder , errorsBuilder , warningsBuilder );
	nodes = nodesBuilder.obj();

	buildGraph( nodes , edgesBuilder , errorsBuilder , warningsBuilder );
	edges = edgesBuilder.obj();

	buildIdMap( nodes , idMapBuilder );
	idMap = idMapBuilder.obj();

	diagnoseSet( nodes , edges , errorsBuilder , warningsBuilder );
	errors = convertToBSON( errorsBuilder );
	warnings = convertToBSON( warningsBuilder );

	resultBuilder.append("nodes" , nodes);   
	resultBuilder.append("edges" , edges);
	resultBuilder.append("idMap" , idMap);
	resultBuilder.append("errors" , errors);
	resultBuilder.append("warnings" , warnings); 
	resultBuilder.append("currentTime" , jsTime() );

//	addNewNodes( conn , nodes );

	BSONObj result = resultBuilder.obj();
	bool written = writeMonitorData( result ); 
	if( written == false ){
	    // TODO: Properly log inability to write to self 
	    cout << "Failed to write PingMonitor data to self" << endl;
	}

	numPings++;
    }

    bool PingMonitor::writeMonitorData( BSONObj& toWrite ){

	//TODO: choose db based on type of mongo instance
	db = "test";
	string writeLocation = db+"."+outerCollection+"."+collectionPrefix+"."+graphs; 

	//TODO: find own HostAndPort more cleanly?
	string selfHostName = getHostName();
	int selfPort = cmdLine.port;
	stringstream ss;
	ss << selfPort;	
	HostAndPort self = selfHostName + ":" + ss.str() ;

	try{
	    ScopedDbConnection conn( self.toString() , socketTimeout); 
	    conn->insert( writeLocation , toWrite );
	    conn.done();
	    numPings++;
	    return true;
	} catch( DBException& e ){
	    return false;
	}
    }

    void PingMonitor::getSetServers( HostAndPort& target , BSONObjBuilder& nodesBuilder , map< string , vector<string> >& errorsBuilder , map< string, vector<string> >& warningsBuilder ){

	scoped_ptr<ScopedDbConnection> connPtr;
	BSONObj cmdReturned;
	try{ 
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    try{
		conn->runCommand( "admin" , BSON("isMaster"<<1) , cmdReturned );	
	    }
	    catch ( DBException& e ){
		// unable to run isMaster command on target
		// TODO: log this error somewhere
		return;
	    } 
	    connPtr->done();
	}
	catch ( UserException ue ){
		// unable to connect to target
		// TODO: log this error somewhere
		return; 
	}

	int id = 0;

	if( cmdReturned["hosts"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["hosts"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
		BSONElement be = *i;
		string curr = be.valuestrsafe();
		BSONObjBuilder newHost;
		newHost.append( "hostName" , curr );
		newHost.append( "machine" , "" );
		newHost.append( "key" , curr + "_" + "" + "_" + "mongod" );
		BSONObj type;
		if( curr.compare(cmdReturned["primary"].valuestrsafe()) == 0 )
		    type = BSON( "process" << "mongod" << "role" << "primary" );
		else
		    type = BSON( "process" << "mongod" << "role" << "secondary" );
		newHost.append( "type" , type );
		collectClientInfo( curr , newHost , type , errorsBuilder , warningsBuilder );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodesBuilder.append( idString , newHost.obj() );
		id++;
	    }
	}	

	if( cmdReturned["passives"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["passives"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
		BSONElement be = *i;
		string curr = be.valuestrsafe();
		BSONObjBuilder newHost;
		newHost.append( "hostName" , curr );
		newHost.append( "machine" , "" );
		newHost.append( "key" , curr + "_" + "" + "_" + "mongod" );
		BSONObj type = BSON( "process" << "mongod" << "role" << "passive" );
		newHost.append( "type" , type );
		collectClientInfo( curr , newHost , type , errorsBuilder , warningsBuilder );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodesBuilder.append( idString , newHost.obj() );
		id++;
	    }
	}	

	if( cmdReturned["arbiters"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["arbiters"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
		BSONElement be = *i;
		string curr = be.valuestrsafe();
		BSONObjBuilder newHost;
		newHost.append( "hostName" , curr );
		newHost.append( "machine" , "" );
		newHost.append( "key" , curr + "_" + "" + "_" + "mongod" );
		BSONObj type = BSON( "process" << "mongod" << "role" << "arbiter" );
		newHost.append( "type" , type );
		collectClientInfo( curr , newHost , type , errorsBuilder , warningsBuilder );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodesBuilder.append( idString , newHost.obj() );
		id++;
	    }
	}	




		
    }

    void PingMonitor::diagnoseSet( BSONObj& nodes , BSONObj& edges , map< string , vector<string> >& errorsBuilder , map<string,vector<string> >& warningsBuilder ){


    }


    void PingMonitor::doPingForCluster(){

	// create a typedef for the Graph type

	// Add nodes to the graph
	
	// Write out the edges in the graph

	// declare a graph object

	// add the edges to the graph object

	BSONObjBuilder resultBuilder;
	resultBuilder.append( "target" , target.toString(true) );


	BSONObjBuilder nodesBuilder, edgesBuilder, idMapBuilder;
	BSONObj nodes, edges, idMap, errors, warnings;
	map<string, vector<string> > errorsBuilder, warningsBuilder;

	int index = 0;

	index = getShardServers( target , nodesBuilder , index , errorsBuilder , warningsBuilder );
	index = getMongosServers( target , nodesBuilder , index , errorsBuilder , warningsBuilder );
	index = getConfigServers( target , nodesBuilder , index , errorsBuilder , warningsBuilder );
	nodes = nodesBuilder.obj();

	buildGraph( nodes , edgesBuilder , errorsBuilder , warningsBuilder );
	edges = edgesBuilder.obj();

	buildIdMap( nodes , idMapBuilder );
	idMap = idMapBuilder.obj();

	diagnose( nodes , edges , errorsBuilder , warningsBuilder );
	errors = convertToBSON( errorsBuilder );
	warnings = convertToBSON( warningsBuilder );

	resultBuilder.append("nodes" , nodes);   
	resultBuilder.append("edges" , edges);
	resultBuilder.append("idMap" , idMap);
	resultBuilder.append("errors" , errors);
	resultBuilder.append("warnings" , warnings); 
	resultBuilder.append("currentTime" , jsTime() );

//	addNewNodes( conn , nodes );

	BSONObj result = resultBuilder.obj();
	bool written = writeMonitorData( result ); 
	if( written == false ){
	    // TODO: Properly log inability to write to self 
	    cout << "Failed to write PingMonitor data to self" << endl;
	}
 
    }

    void PingMonitor::addNewNodes( HostAndPort& target , BSONObj& nodes ){
	BSONObjBuilder allNodes;
    
	for( BSONObj::iterator i=nodes.begin(); i.more(); ){
	    BSONElement nodeElem = i.next();
	    BSONObj nodeObj = nodeElem.embeddedObject();
	    BSONObjBuilder nodeFields;
	    nodeFields.append( "currentRole" , nodeObj.getObjectField("type")["role"].valuestrsafe() );
	    //nodeSubObj.appendArray( "previousRoles" )

	    BSONObjBuilder qBuilder;
	    qBuilder.append( nodeObj["key"].valuestrsafe() , nodeFields.obj() );
	    qBuilder.append("upsert" , true );
	    Query q( qBuilder.obj() ); 

	   // conn.update("test.pingmonitor.allnodes" , q );	    
	}	
    }

    BSONObj convertToBSON( map<string, vector<string> >& m ){
	BSONObjBuilder b;
	for( map<string, vector<string> >::iterator mapItr = m.begin(); mapItr!=m.end(); ++mapItr){
	    BSONArrayBuilder aBuilder;
	    for( vector<string>::iterator vecItr = mapItr->second.begin(); vecItr!=mapItr->second.end(); vecItr++){
		aBuilder.append( *vecItr );	
	    }
	   b.append( mapItr->first , aBuilder.arr() ); 
	} 
	return b.obj();
    }

    void PingMonitor::collectClientInfoHelper( HostAndPort& hp , BSONObjBuilder& newHost , const string& db , string cmdName ){
	BSONObj cmdReturned;
	try{
	    ScopedDbConnection conn( hp.toString() );
	    conn->runCommand( db, BSON(cmdName<<1) , cmdReturned );
	    newHost.append( cmdName , cmdReturned );
	    conn.done();
	}
	catch( DBException& e){
	    newHost.append( cmdName , e.toString() );
	}
    }

    void PingMonitor::collectClientInfo( string connString , BSONObjBuilder& newHost , BSONObj& type , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){	

	HostAndPort hp( connString );

	string mongod = "mongod";
	string primary = "primary";
	string secondary = "secondary";

	//all nodes collect the following
	collectClientInfoHelper( hp , newHost , "admin" , "hostInfo" );
	collectClientInfoHelper( hp , newHost , "admin" , "serverStatus" );
	collectClientInfoHelper( hp , newHost , "admin" , "shardVersion" );

	// only mongod instances collect the following
	if( mongod.compare(type.getStringField("role")) == 0 )
	    collectClientInfoHelper( hp , newHost , "admin" , "buildInfo" );

	// only shard mongod instances collect the following
	if ( primary.compare(type.getStringField("role")) == 0 || secondary.compare(type.getStringField("role")) == 0 )
	    collectClientInfoHelper( hp , newHost , "admin" , "isMaster" );
    }

    int PingMonitor::getShardServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    scoped_ptr<DBClientCursor> cursor( conn->query( "config.shards" , BSONObj() ) ) ;
	    while( cursor->more() ){
		BSONObj p = cursor->next();
		string hostField = p.getStringField("host");
		string::size_type startPos = hostField.find("/");
		if( startPos != string::npos ){
		    string shardName = hostField.substr(0, startPos); 
		    string memberHosts = hostField.substr(startPos + 1);
		    string delimiter = ",";
		    int count = 0;
		    size_t currPos = 0;
		    string currToken;	
		    while( (currPos = memberHosts.find(delimiter)) != string::npos ){
			currToken = memberHosts.substr(0 , currPos);
			int id = index;
			index++;
			BSONObjBuilder newHost;
			newHost.append( "hostName" , currToken );
			newHost.append( "machine" , "" );
			newHost.append( "shardName" , shardName );
			newHost.append( "key" , currToken + "_" + "" + "_" + "mongod" );
			BSONObj type;	
			if( count == 0 )
			    type = BSON( "process" << "mongod" << "role" << "primary" );
			else
			    type = BSON( "process" << "mongod" << "role" << "secondary" );
			newHost.append( "type" , type );
			collectClientInfo( currToken , newHost , type , errors , warnings );
			char idString[100];
			sprintf(idString , "%d" , id);
			nodes.append( idString , newHost.obj() );
			memberHosts.erase(0, currPos + delimiter.length());
			count++; 
		    }			
		    int id = index;
		    index++;
		    BSONObjBuilder newHost;
		    newHost.append( "hostName" , memberHosts );
		    newHost.append( "machine" , "" );
		    newHost.append( "shardName" , shardName );
		    newHost.append( "key" , memberHosts + "_" + "" + "_" + "mongod" );
		    BSONObj type = BSON( "process" << "mongod" << "role" << "secondary" );
		    newHost.append( "type" , type );
		    collectClientInfo( HostAndPort(currToken) , newHost , type , errors , warnings );
		    char idString[100];
		    sprintf(idString , "%d" , id);
		    nodes.append( idString , newHost.obj() );
		}
		else{
		    int id = index;
		    index++;
		    BSONObjBuilder newHost;
		    newHost.append( "hostName" , p.getStringField("host") );
		    newHost.append( "machine" , "" );
		    newHost.append( "key" , hostField + "_" + "" + "_" + "mongod" );
		    BSONObj type = BSON( "process" << "mongod" << "role" << "primary" );
		    collectClientInfo( HostAndPort(p.getStringField("host")) , newHost , type , errors , warnings );
		    char idString[100];
		    sprintf(idString , "%d" , id);
		    nodes.append( idString , newHost.obj() );
		} 
	    }
	}
	catch( DBException& e ){}

	connPtr->done();
	return index;
    }

    int PingMonitor::getMongosServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	auto_ptr<DBClientCursor> cursor;
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    scoped_ptr<DBClientCursor> cursor( conn->query( "config.mongos" , BSONObj() ) ) ;
	    while( cursor->more()){
		BSONObj p = cursor->next();
		string hostField = p.getStringField("_id");
		int id = index;
		index++;
		BSONObjBuilder newHost;
		newHost.append( "hostName" , hostField );
		newHost.append( "machine" , "" );
		newHost.append( "key" , hostField + "_" + "" + "_" + "mongos" );
		BSONObj type = BSON( "process" << "mongos" << "role" << "mongos" );
		newHost.append( "type" , type );
		collectClientInfo( hostField , newHost , type , errors , warnings );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodes.append( idString , newHost.obj() );
	    }
	} catch( DBException& e) {} 
	connPtr->done();
	return index;
    }

    int PingMonitor::getConfigServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    BSONObj cmdReturned;
	    conn->runCommand( "admin" , BSON("getCmdLineOpts"<<1) , cmdReturned );
	    string host = cmdReturned["parsed"]["configdb"].valuestrsafe();
	    int id = index;
	    index++;
	    BSONObjBuilder newHost;
	    newHost.append( "hostName" , host );
	    newHost.append( "machine" , "" );
	    newHost.append( "key" , host + "_" + "" + "_" + "mongod" );
	    BSONObj type = BSON( "process" << "mongod" << "role" << "config" );
	    newHost.append( "type" , type );
	    collectClientInfo( host , newHost , type , errors , warnings );
	    char idString[100];
	    sprintf(idString, "%d", id);
	    nodes.append( idString, newHost.obj() ); 
	} 
	catch( DBException& e) {}
	connPtr->done();
	return index;
    }

    void PingMonitor::buildGraph( BSONObj& nodes , BSONObjBuilder& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	set< string > srcNodeIds;
	set< string > tgtNodeIds;
	nodes.getFieldNames( srcNodeIds );
	nodes.getFieldNames( tgtNodeIds );

	for (set<string>::iterator srcId = srcNodeIds.begin(); srcId!=srcNodeIds.end(); ++srcId){
	    BSONElement srcElem = nodes[ *srcId ];
	    BSONObj srcObj = srcElem.embeddedObject();
	    BSONObjBuilder srcEdges;
	    string srcHostName = srcObj.getStringField("hostName"); 
	    for( set<string>::iterator tgtId = tgtNodeIds.begin(); tgtId!=tgtNodeIds.end(); ++tgtId ){
		BSONElement tgtElem = nodes[ *tgtId ];
		BSONObj tgtObj = tgtElem.embeddedObject();
		BSONObjBuilder newEdge;
    		string tgtHostName = tgtObj.getStringField("hostName"); 
		scoped_ptr<ScopedDbConnection> connPtr;
		BSONObj cmdReturned;
		try{ 
		    connPtr.reset( new ScopedDbConnection( srcHostName , socketTimeout ) );
		    ScopedDbConnection& conn = *connPtr;
		    try{
			BSONObj hostArray = BSON( "0" << tgtHostName );
			BSONObj pingCmd = BSON(  "ping" << 1 << "hosts" << hostArray ); 
			conn->runCommand( "admin" , pingCmd , cmdReturned );	
		    }
		    catch ( DBException& e ){ 
			newEdge.append( "isConnected" , "false" );
			newEdge.append( "errmsg" , /*TODO*/" " );
			continue;
		    } 
		    connPtr->done();
		}
		catch( UserException& e ){
		    //TODO: log client connection error
		}
		BSONObj edgeInfo = cmdReturned.getObjectField( tgtHostName );
		if( edgeInfo["isConnected"].boolean() == false ){
		    newEdge.append( "isConnected" , false );
		    newEdge.append( "errmsg" , edgeInfo["errmsg"].valuestrsafe() );
		}
		else{
		    newEdge.append( "isConnected" , true );
		    newEdge.append( "pingTimeMicrosecs" , edgeInfo["pingTimeMicrosecs"].valuestrsafe() );
		}
		newEdge.append( "bytesSent" , edgeInfo["bytesSent"]._numberLong() );
		newEdge.append( "bytesReceived" , edgeInfo["bytesReceived"]._numberLong() );
		newEdge.append( "incomingSocketExceptions" , edgeInfo["incomingSocketExceptions"]._numberLong() );
		newEdge.append( "outgoingSocketExceptions" , edgeInfo["outgoingSocketExceptions"]._numberLong() );
		srcEdges.append( tgtElem.fieldName() , newEdge.obj() );  
	    }
	    edges.append( srcElem.fieldName() , srcEdges.obj() );
	}
    }

    void PingMonitor::buildIdMap( BSONObj& nodes , BSONObjBuilder& idMap ){
	for(BSONObj::iterator i = nodes.begin(); i.more(); ){
	    BSONElement nodeElem = i.next();
	    BSONObj nodeObj = nodeElem.embeddedObject();
	    idMap.append( nodeObj["key"].valuestrsafe() , nodeElem.fieldName() ); 
	}
    }

    void PingMonitor::initializeCharts(){
	BSONObjBuilder req;
	BSONObj req_mongos = BSONObjBuilder().append("mongos",false ).append("config",true).append("primary",true).append("secondary",false).obj();
	BSONObj req_config = BSONObjBuilder().append("mongos",true ).append("config",true).append("primary",true).append("secondary",false).obj();
	BSONObj req_primary = BSONObjBuilder().append("mongos",true ).append("config",true).append("primary",true).append("secondary",false).obj();
	BSONObj req_secondary = BSONObjBuilder().append("mongos",false ).append("config",false).append("primary",false).append("secondary",false).obj();
	req.append("mongos", req_mongos);
	req.append("config", req_config);
	req.append("primary", req_primary);
	req.append("secondary", req_secondary);
	reqConnChart = req.obj();

	BSONObjBuilder rec;
	BSONObj rec_mongos = BSONObjBuilder().append("mongos",false ).append("config",false).append("primary",false).append("secondary",true).obj();
	BSONObj rec_config = BSONObjBuilder().append("mongos",false ).append("config",false).append("primary",false).append("secondary",true).obj();
	BSONObj rec_primary = BSONObjBuilder().append("mongos",false ).append("config",false).append("primary",false).append("secondary",true).obj();
	BSONObj rec_secondary = BSONObjBuilder().append("mongos",true ).append("config",true).append("primary",true).append("secondary",true).obj();
	rec.append("mongos", rec_mongos);
	rec.append("config", rec_config);
	rec.append("primary", rec_primary);
	rec.append("secondary", rec_secondary);
	recConnChart = rec.obj();
    } 
  
    bool PingMonitor::isReqConn( const string& src , const string& tgt){
	return reqConnChart.getObjectField( src )[ tgt ].boolean();
    }

    bool PingMonitor::isRecConn( const string& src , const string& tgt ){
	return recConnChart.getObjectField( src )[ tgt ].boolean();
    }

    void PingMonitor::addError( const string& key , const string& err , map<string, vector<string> >& errors){
	vector<string> list = errors[ key ];
	vector<string>::iterator it;
	if( (it = find( list.begin(), list.end(), err )) == list.end() )
	    list.push_back( err );
	errors[ key ] = list;
    }

    void PingMonitor::addWarning(const string& key , const string& warn , map<string, vector<string> >& warnings){  
	vector<string> list = warnings[ key ];
	vector<string>::iterator it;
	if( (it = find( list.begin(), list.end(), warn )) == list.end() )
	    list.push_back( warn );
	warnings [ key ] = list;
    }
 
    void PingMonitor::diagnose( BSONObj& nodes , BSONObj& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	for (BSONObj::iterator i = nodes.begin(); i.more(); ){
	    BSONElement srcElem = i.next();
	    BSONObj srcNodeInfo = srcElem.embeddedObject();
	    for( BSONObj::iterator j = nodes.begin(); j.more(); ){
		BSONElement tgtElem = j.next(); 
		BSONObj tgtNodeInfo = tgtElem.embeddedObject();
		BSONObj edge = edges.getObjectField( srcElem.fieldName() ).getObjectField( tgtElem.fieldName() );
		if ( edge["isConnected"].boolean() == false ){
		    if( isReqConn( srcNodeInfo.getObjectField("type")["role"].valuestrsafe() , tgtNodeInfo.getObjectField("type")["role"].valuestrsafe() ) ){
			string err( ERRCODES["MISSING_REQ_CONN"] );
			err += tgtNodeInfo["hostName"].valuestrsafe();	
			addError( srcNodeInfo["key"].valuestrsafe() , err , errors );
		    }
		    if( isRecConn( srcNodeInfo.getObjectField("type")["role"].valuestrsafe() , tgtNodeInfo.getObjectField("type")["role"].valuestrsafe() ) ){
			string warn( ERRCODES["MISSING_REC_CONN"] );
			warn += tgtNodeInfo["hostName"].valuestrsafe();
			addWarning( srcNodeInfo["key"].valuestrsafe() , warn , warnings ); 
		    }
		}
	    }
	}
    }

/*    BSONObj PingMonitor::calculateStats(){
	BSONObjBuilder results;
*/    /* fields
	numPingAttempts
	numSuccessful
	numFailed
	percentageIsConnected
	avgIncomingSocketExceptions
	avgOutgiongSocketExceptions
	avgBytesSent
	avgBytesReceived
	maxPingTimeMicrosecs
	minPingTimeMicrosecs
	avgPingTimeMicrosecs
	pingTimeStdDeviation	
    */

    /* auxiliary fields
	sumPingTimeMicrosecs
	subtractMeanSquaredSum
    */

/*

	DBClientConnection conn;
	string connInfo;
	bool isConnected;
	BSONObj monitorResults;
	try{
	    isConnected = conn.connect( target , connInfo );
	    if( conn.count("test.pingMonitor") < 1 ){
		results.append("errmsg" , ERRCODES["NOT_ENOUGH_RECORDS"]);
		return results.obj();
	    }
	    auto_ptr<DBClientCursor> cursor  = conn.query( "test.pingmonitor" , Query(BSONObj()).sort("_id",-1) );
	   
	    while( cursor->more() ){ 
		BSONObj moment = cursor->next();
		BSONObj edges = moment["edges"].embeddedObject();	
			    
		//max and min ping time, num ping attempts, num successful, num failed, num socket exceptions, num bytes exchanged
		for (BSONObj::iterator i = monitorResults.getObjectField("edges").begin(); i.more(); ){
		    const BSONElement srcElem = i.next();
		    const BSONObj srcObj = srcElem.embeddedObject();
		    for( BSONObj::iterator j = srcObj.begin(); j.more(); ){
			const BSONElement tgtElem = j.next();
			const BSONObj tgtObj = tgtElem.embeddedObject();
			string edgestr = "";
			edgestr += srcElem.fieldName();
			edgestr += " -> ";
			edgestr += tgtElem.fieldName();
			edgestr += tgtObj["pingTimeMicrosecs"].valuestrsafe();
			edgeShow.append( edgestr , tgtObj["isConnected"].boolean() );
		    }
		}
	   /


	    }
	} catch( DBException& e ){ TODO }

	return results.obj();

    }
*/

    void PingMonitor::shutdown(){
	alive = false;
	bool threadDone = false;
	int count = 0;
	while( threadDone == false && count < 10 ){
	    threadDone = wait( 1000 );
	    count++;
	} 
    }

    void PingMonitor::run() {
	lastPingNetworkMillis = 0;
	while ( ! inShutdown() && alive ) {
	    LOG(3) << "PingMonitor thread for " << target.toString() << "  awake" << endl;

	    /*
	    if( lockedForWriting() ) {
		// note: this is not perfect as you can go into fsync+lock between
		// this and actually doing the delete later
		LOG(3) << " locked for writing" << endl;
		continue;
	    }
	    */

	    //ping the network and time it
	    using namespace boost::posix_time;
	    ptime time_start(microsec_clock::local_time());
	    /*>>>*/ doPingForTarget(); /*<<<*/
	    ptime time_end(microsec_clock::local_time());
	    time_duration duration(time_end - time_start);
	    lastPingNetworkMillis = duration.total_milliseconds();
	    
    	    sleepsecs( interval );
	}
    }

} 
