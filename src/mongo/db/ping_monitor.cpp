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
#include <istream>
#include <climits>
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
    const string PingMonitor::allNodes = "allNodes";

    // Error codes
    map< string , string > PingMonitor::ERRCODES = PingMonitor::initializeErrcodes();
    map< string , string > PingMonitor::initializeErrcodes(){
	map<string,string> m;
	m["MISSING_REQ_CONN"] = "Missing required connection to ";
	m["MISSING_REC_CONN"] = "Missing recommended connection to ";
	m["TARGET_NOT_NETWORK_MASTER"] = "Target is not the master of a network (primary of replica set, mongos instance, or master in master-slave relationship).";
	m["NOT_ENOUGH_RECORDS"] = "Not enough PingMonitor records to complete this action.";
	m["CAN'T_MAKE_CLIENT_CONNECTION"] = "Cannot make client connection to host.";
	m["NO_DATA"] = "No ping monitor data has been collected for this target.";
	return m;
    };

    // Retrieval commands 

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

    void PingMonitor::clearHistory(){
	try{
	    ScopedDbConnection conn( self.toString() , socketTimeout );
	    conn->query( graphsLocation , Query( BSON( "drop" << 1 ) ) ); 
	    conn.done();
	}
	catch( DBException& e){
	    cout << "[PingMonitor::clearHistory()] : " << e.toString() << endl;
	}
    }

   
    // Retrieve from ping monitor storage database
    BSONObj PingMonitor::getMonitorInfo(){ 
	BSONObjBuilder toReturn;
	BSONObj latestMonitorInfo;
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() , socketTimeout ) ); 
	    ScopedDbConnection& conn = *connPtr;
	    // sort snapshots in reverse chronological order
	    // to ensure the first returned is the lastest
	    scoped_ptr<DBClientCursor> cursor( conn->query( graphsLocation , Query(BSONObj()).sort("_id",-1)) ) ;
	    connPtr->done(); 
	    if( cursor->more() )
		latestMonitorInfo = cursor->nextSafe();
	    else{
		toReturn.append( "errmsg" , ERRCODES["NO_DATA"] );
		return toReturn.obj();
	    }
	}
	catch( DBException& e ){
	    toReturn.append("errmsg" , e.toString() );
	    return toReturn.obj();	    	
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
//    	toReturn.append( "edges_full" , latestMonitorInfo.getObjectField("edges") ); 
//	toReturn.append( "nodes_full" , latestMonitorInfo.getObjectField("nodes") );
	toReturn.append( "edges" , edgeShow.obj() );
	toReturn.append( "nodes" , nodeShow.obj() );
	toReturn.append( "errors" , latestMonitorInfo.getObjectField("errors") );
	toReturn.append( "warnings" , latestMonitorInfo.getObjectField("warnings") );
	return toReturn.obj(); 
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

//	diagnoseSet( nodes , edges , errorsBuilder , warningsBuilder );
	errors = convertToBSON( errorsBuilder );
	warnings = convertToBSON( warningsBuilder );

	resultBuilder.append("nodes" , nodes);   
	resultBuilder.append("edges" , edges);
	resultBuilder.append("idMap" , idMap);
	resultBuilder.append("errors" , errors);
	resultBuilder.append("warnings" , warnings); 
	resultBuilder.append("currentTime" , jsTime() );

	addNewNodes( nodes );
	
	//TODO: choose db based on type of mongo instance
	db = "test";

	BSONObj result = resultBuilder.obj();
	bool written = writeMonitorData( result ); 
	if( written == false ){
	    // TODO: Properly log inability to write to self 
	    cout << "[PingMonitor::doPingForReplset()] : " << "Failed to write PingMonitor data from " + target.toString() + " to self on " + graphsLocation << endl;
	}

	calculateStats();

	numPings++;
    }

    bool PingMonitor::writeMonitorData( BSONObj& toWrite ){
	try{
	    ScopedDbConnection conn( self.toString() , socketTimeout); 
	    conn->insert( graphsLocation , toWrite );
	    conn.done();
	    numPings++;
	    return true;
	} catch( DBException& e ){
	    cout << "[PingMonitor::writeMonitorData()] : " << e.toString() << endl;
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
	catch ( DBException& ue ){
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

	addNewNodes( nodes );
	
	BSONObj result = resultBuilder.obj();
	bool written = writeMonitorData( result ); 
	if( written == false ){
	    // TODO: make error an errcode 
	    cout << "[PingMonitor::doPingForCluster()] : " << "Failed to write PingMonitor data to self" << endl;
	}
 
	calculateStats();

    }

    void PingMonitor::addNewNodes( BSONObj& nodes ){
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() , socketTimeout ) ); 
	    ScopedDbConnection& conn = *connPtr;
	    for( BSONObj::iterator i=nodes.begin(); i.more(); ){
		BSONElement nodeElem = i.next();
		BSONObj nodeObj = nodeElem.embeddedObject();
		string nodeKey = nodeObj["key"].valuestrsafe();
		scoped_ptr<DBClientCursor> cursor( conn->query( allNodesLocation , QUERY("key" << nodeKey ) ) );
		if( cursor->more() ){
		    // node is already in allNodes
		    // potentially add more information such as previous roles, uptime etc
		    //BSONObjBuilder nodeFields;
		    //nodeFields.append( "currentRole" , nodeObj.getObjectField("type")["role"].valuestrsafe() );
		    //nodeSubObj.appendArray( "previousRoles" )
		    // maybe associate role with timestamp? timeStamp : role , timeStamp : role
		}
		else{
		    // need to add node to allNodes
		    BSONObj newNode = BSON( "key" << nodeObj["key"].valuestrsafe() );
		    conn->insert( allNodesLocation , newNode );
		}
	    }	
	connPtr->done();	
	}
	catch( DBException& e ){
	    cout << "[PingMonitor::addNewNodes] : " << e.toString() << endl;
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
	    auto_ptr<DBClientCursor> cursor( conn->query( "config.shards" , BSONObj() ) ) ;
	    while( cursor->more() ){
		BSONObj p = cursor->nextSafe();
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
	catch( DBException& e ){
	    cout << "[PingMonitor::getShardServers()] : " << e.toString() << endl;
	}

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
		BSONObj p = cursor->nextSafe();
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
	} catch( DBException& e) {
	    cout << "[PingMonitor::getShardServers()] : " << e.toString() << endl;
	}
	connPtr->done();
	return index;
    }

    //TODO : only captures a single config server
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
	catch( DBException& e) {
	    cout << "[PingMonitor::getShardServers()] : " << "Couldn't connect to config server" << endl;
	}
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
		BSONObj cmdReturned;
		try{ 
		    ScopedDbConnection conn( srcHostName , socketTimeout ); 
		    try{
			BSONObjBuilder pingCmdBuilder;
			pingCmdBuilder.append( "ping" , 1 );
			BSONObj hostObj = BSON( "0" << tgtHostName );
			pingCmdBuilder.appendArray( "hosts" , hostObj );
			BSONObj pingCmd = pingCmdBuilder.obj();
			conn->runCommand( "admin" , pingCmd , cmdReturned );	
		    }
		    catch ( DBException& e ){ 
			newEdge.append( "isConnected" , "false" );
			newEdge.append( "errmsg" , e.toString() );
			continue;
			cout << "[PingMonitor::buildGraph()] : " << e.toString() << endl;
		    } 
		    conn.done();
		}
		catch( DBException& e ){
		    cout << "[PingMonitor::buildGraph()] : " << e.toString() << endl;
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
		newEdge.append( "bytesSent" , edgeInfo["bytesSent"].numberLong() );
		newEdge.append( "bytesReceived" , edgeInfo["bytesReceived"].numberLong() );
		newEdge.append( "incomingSocketExceptions" , edgeInfo["incomingSocketExceptions"].numberLong() );
		newEdge.append( "outgoingSocketExceptions" , edgeInfo["outgoingSocketExceptions"].numberLong() );
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

    void PingMonitor::calculateStats(){
	scoped_ptr<ScopedDbConnection> connPtr;
	auto_ptr<DBClientCursor> cursor;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() ) ); 
	    ScopedDbConnection& conn = *connPtr;
	    cursor = conn->query( graphsLocation , Query(BSONObj()) );
	    if( cursor->more() == false ){
		// no graphs data to read from
		// TODO: do we need to log this?
		// ERRCODES["NO_DATA"]
		connPtr->done();
		return;
	    }
	    else{
		
		// get a list of all node keys as strings
		auto_ptr<DBClientCursor> allNodesCursor = conn->query( allNodesLocation, Query(BSONObj()) );
		vector< string > allNodesList;
		while( allNodesCursor->more() )
		    allNodesList.push_back( allNodesCursor->next()["key"].valuestrsafe() );

		// initialize maps
		// first one is a three dimensional map of ping times to edges at every point in time
		map< int , map< string , map<string, int> > > pingTime; // only used for intermediate steps;
	    	map< string , map<string, int> > numPingAttempts;
		map< string , map<string, int> > numSuccessful;
		map< string , map<string, int> > numFailed;
		map< string , map<string, double> > percentConnected;
		map< string , map<string, int> > maxPingTime;
		map< string , map<string, int> > minPingTime;
		map< string , map<string, long long> > sumPingTime; //only used for intermediate steps
		map< string , map<string, double> > avgPingTime;
		map< string , map<string, double> > pingTimeStdDev;
		map< string , map<string, double> > subtractMeanSquaredSum ; // only used for intermediate steps
		map< string , map<string, long long> > totalOutSocketExceptions;
		map< string , map<string, long long> > totalInSocketExceptions; 
		map< string , map<string, long long> > totalBytesSent;
		map< string , map<string, long long> > totalBytesRecd;

		// initialize values (many need to be compared) 
		for( vector<string>::iterator src=allNodesList.begin(); src!=allNodesList.end(); ++src ){
		    for( vector<string>::iterator tgt=allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			if( (*src).compare( *tgt ) != 0 ){
			    maxPingTime[ *src ][ *tgt ] = 0;
			    minPingTime[ *src ][ *tgt ] = INT_MAX;	    
			    sumPingTime[ *src ][ *tgt ] = 0;
			    numPingAttempts[ *src ][ *tgt ] = 0;
			    numSuccessful[ *src ][ *tgt ] = 0;
			    numFailed[ *src ][ *tgt ] = 0;
			    percentConnected[ *src ][ *tgt ] = 0;
			    sumPingTime[ *src ][ *tgt ] = 0;
			    avgPingTime[ *src ][ *tgt ] = 0;
			    pingTimeStdDev[ *src ][ *tgt ] = 0;
			    subtractMeanSquaredSum[ *src ][ *tgt ] = 0;
			    totalOutSocketExceptions[ *src ][ *tgt ] = 0;
			    totalInSocketExceptions[ *src ][ *tgt ] = 0;
			    totalBytesSent[ *src ][ *tgt ] = 0;
			    totalBytesRecd[ *src ][ *tgt ] = 0;
			}
		    }
		}

		// create a temporary map of edges to ping times for quick access in calculating stats
		// pingTime[ *src ][ *tgt ] = 0 means nodes not connected
		// pingTime[ *src ][ *tgt ] = -1 means edge does not exist, either because it's a
		// self-self edge, or those two nodes were not present at the same time
		// also, calculate simple additive stats for socket exceptions and bytes sent/recd
		int count = 0;
		while( cursor->more() ){
		    BSONObj curr = cursor->nextSafe();
		    BSONObj currEdges = curr.getObjectField("edges"); 
		    BSONObj currIds = curr.getObjectField("idMap");
		    for( vector<string>::iterator src=allNodesList.begin(); src!=allNodesList.end(); ++src ){
			for( vector<string>::iterator tgt=allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			    if( (*src).compare( *tgt ) != 0 && currIds[ *src ].trueValue() != false && currIds[ *tgt ].trueValue() != false){
				string srcNum = currIds[ *src ].valuestrsafe();
				string tgtNum = currIds[ *tgt ].valuestrsafe();
				BSONObj edge = currEdges.getObjectField( srcNum ).getObjectField( tgtNum );
				// ping time
				if( edge["pingTimeMicrosecs"].trueValue() )
				    pingTime[ count ][ *src ][ *tgt ] = atoi( edge["pingTimeMicrosecs"].valuestrsafe() );	
				else
				    pingTime[ count ][ *src ][ *tgt ] = 0;
				// socket exceptions and bytes sent/recd
				totalOutSocketExceptions[ *src ][ *tgt ] += edge["outgoingSocketExceptions"].numberLong();
				totalInSocketExceptions[ *src ][ *tgt ] += edge["incomingSocketExceptions"].numberLong();
				totalBytesSent[ *src ][ *tgt ] += edge["bytesSent"].numberLong();
				totalBytesRecd[ *src ][ *tgt ] += edge["bytesReceived"].numberLong();
			    }
			    else
				pingTime[ count ][ *src ][ *tgt ] = -1;
			}
		    }
		    count++;
		}

		// calculate stats requiring one level of depth (num ping attempts, successful and
		// failed; max and min ping times
		for( int i=0; i<count; i++){
		    for( vector<string>::iterator src = allNodesList.begin(); src!=allNodesList.end(); ++src ){
			for( vector<string>::iterator tgt = allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			    int p = pingTime[ i ][ *src ][ *tgt ];
			    // the nodes were present at the same time
			    if( p >= 0 ){ 
				numPingAttempts[ *src ][ *tgt ]++;
				// the nodes were connected
				if( p > 0 ){
				    numSuccessful[ *src ][ *tgt ]++;
				    if( p > maxPingTime[ *src ][ *tgt ] )
					maxPingTime[ *src ][ *tgt ] = p;
				    if( p < minPingTime[ *src ][ *tgt ] )
					minPingTime[ *src ][ *tgt ] = p;
				    sumPingTime[ *src ][ *tgt ] += p;
				}
				else
				    numFailed[ *src ][ *tgt ]++;
			    }
			} 
		    }
		}

		// calculate stats requiring two levels of depth (percent connected, average ping time)
		for( vector<string>::iterator src = allNodesList.begin(); src!=allNodesList.end(); ++src ){
		    for( vector<string>::iterator tgt = allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			// the nodes were distinct
			if( numPingAttempts[ *src ][ *tgt ] > 0){
			    if( numSuccessful[ *src ][ *tgt ] != 0 ){
				double numSuccessfulD = numSuccessful[ *src ][ *tgt ];	
				avgPingTime[ *src ][ *tgt ] = sumPingTime[ *src ][ *tgt ] / numSuccessfulD;
			    }
			    double numPingAttemptsD = numPingAttempts[ *src ][ *tgt ];
			    percentConnected[ *src ][ *tgt ] = 100 * numSuccessful[ *src ][ *tgt ] / numPingAttemptsD;
		    	}	
		    } 
		}

		// calculate stats requiring three and four levels of depth (ping time standard deviation)
		for( int i=0; i<count; i++){
		    for( vector<string>::iterator src = allNodesList.begin(); src!=allNodesList.end(); ++src ){
			for( vector<string>::iterator tgt = allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			    int p = pingTime[ i ][ *src ][ *tgt ];
			    // the nodes were connected
			    if( p > 0)
				subtractMeanSquaredSum[ *src ][ *tgt ] += ( p - avgPingTime[ *src ][ *tgt ] ) * ( p - avgPingTime[ *src ][ *tgt ] ); 
			} 
		    }
		}
		for( vector<string>::iterator src = allNodesList.begin(); src!=allNodesList.end(); ++src ){
		    for( vector<string>::iterator tgt = allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			if( subtractMeanSquaredSum[ *src ][ *tgt ] > 0)
			    pingTimeStdDev[ *src ][ *tgt ] = sqrt( subtractMeanSquaredSum[ *src ][ *tgt ] / numSuccessful[ *src ][ *tgt ] );
		    } 
		}

		// save all stats figures to the database
		BSONObjBuilder statsBuilder;
		for( vector<string>::iterator src=allNodesList.begin(); src!=allNodesList.end(); ++src ){
		    BSONObjBuilder srcBuilder;
		    for( vector<string>::iterator tgt=allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			if( (*src).compare( *tgt ) != 0 ){
			    BSONObjBuilder tgtBuilder;
			    tgtBuilder.append( "numPingAttempts" , numPingAttempts[ *src ][ *tgt ] );
			    tgtBuilder.append( "numSuccessful" , numSuccessful[ *src ][ *tgt ] );
			    tgtBuilder.append( "numFailed" , numFailed[ *src ][ *tgt ] );
			    tgtBuilder.append( "percentConnected" , percentConnected[ *src ][ *tgt ] ); 
			    tgtBuilder.append( "maxPingTime" , maxPingTime[ *src ][ *tgt ] );
			    if( minPingTime[ *src ][ *tgt ] == INT_MAX )
				tgtBuilder.append( "minPingTime" , "INT_MAX" );
			    else
				tgtBuilder.append( "minPingTime" , minPingTime[ *src ][ *tgt ] );
			    tgtBuilder.append( "avgPingTime" , avgPingTime[ *src ][ *tgt ] );
			    tgtBuilder.append( "pingTimeStdDev" , pingTimeStdDev[ *src ][ *tgt ] ); 
			    tgtBuilder.append( "totalOutSocketExceptions" , totalOutSocketExceptions[ *src ][ *tgt ] );
			    tgtBuilder.append( "totalInSocketExceptions" , totalInSocketExceptions[ *src ][ *tgt ] ); 
			    tgtBuilder.append( "totalBytesSent" , totalBytesSent[ *src ][ *tgt ] );
			    tgtBuilder.append( "totalBytesRecd" , totalBytesRecd[ *src ][ *tgt ] );
			    srcBuilder.append( *tgt , tgtBuilder.obj() );
			}
		    }
		    statsBuilder.append( *src , srcBuilder.obj() );
		}
		conn->insert( statsLocation , statsBuilder.obj() ); 
	    }
	    connPtr->done(); 

	}
	catch( DBException& e ){
	    cout << "[PingMonitor::calculateStats()] : " << e.toString() << endl;
	}
    }


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
	    cout << "[PingMonitor::run()] : " << "PingMonitor thread for " << target.toString() << "  awake" << endl;

	    /*
	    if( lockedForWriting() ) {
		// note: this is not perfect as you can go into fsync+lock between
		// this and actually doing the delete later
		cout << " locked for writing" << endl;
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
	clearHistory();
    }

} 
