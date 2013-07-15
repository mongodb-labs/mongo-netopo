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
#include <boost/tokenizer.hpp>
#include <unistd.h>


namespace mongo {

    // PingMonitor

    boost::mutex PingMonitor::_mutex;

    bool PingMonitor::isMonitoring = false;
    bool PingMonitor::targetIsSet = false;
    int PingMonitor::pingInterval = 15; // 15 seconds 
    bool PingMonitor::setPingInterval( int nsecs ){ pingInterval = nsecs; return true; }
    int PingMonitor::getPingInterval(){ return pingInterval; }
    BSONObj PingMonitor::reqConnChart;
    BSONObj PingMonitor::recConnChart;
 

    // Error codes

    map< string , string> PingMonitor::ERRCODES = PingMonitor::initializeErrcodes();
    map<string,string> PingMonitor::initializeErrcodes(){
	map<string,string> m;
	m["MISSING_REQ_CONN"] = "Missing required connection to ";
	m["MISSING_REC_CONN"] = "Missing recommended connection to ";
	m["TARGET_NOT_NETWORK_MASTER"] = "Target is not the master of a network (primary of replica set, mongos instance, or master in master-slave relationship).";
	m["NOT_ENOUGH_RECORDS"] = "Not enough PingMonitor records to complete this action.";
	return m;
    };

    
    // Commands for getting and setting target

    HostAndPort PingMonitor::target;
    string PingMonitor::targetNetworkType;

    // always check if target is set before using 
    HostAndPort PingMonitor::getTarget(){
	return target; 
    } 

    bool PingMonitor::getTargetIsSet(){
	return targetIsSet;
    }

    // returns false if target is invalid (not of master in a network system,
    // not running a mongod or mongos instance, etc )
    BSONObj PingMonitor::setTarget( HostAndPort hp ){
	BSONObjBuilder toReturn;
	BSONObj connStatus = canConnect( hp );
	if( connStatus["isConnected"].boolean() ){
	    if( connStatus["networkOk"].boolean() ){
		target = hp;
		targetIsSet = true;
		toReturn.append("ok" , true);
		return toReturn.obj();
	    }
	    else{
		// target is not a replica set primary or mongos instance
		toReturn.append("ok" , false);
		toReturn.append("errmsg" , ERRCODES["TARGET_NOT_NETWORK_MASTER"] );
	    }
	}
	else{
	    // can't make client connection to requested target
	    // connStatus["connectionMsg"] contains message from connect()
	    toReturn.append("ok" , false );
	    toReturn.append("errmsg" , connStatus["connectionMsg"] );
	}
	return toReturn.obj();	    
    }

    // always check if target is set before using
    string PingMonitor::getTargetNetworkType(){
	return targetNetworkType;
    }


    // Monitoring commands

    bool PingMonitor::getIsMonitoring(){
	return isMonitoring;
    } 

    // stops monitoring process but does not reset target or clear monitoring history
    bool PingMonitor::turnOffMonitoring(){
	isMonitoring = false;
	return true;
    }

    // turns on monitoring process if target had been previously set 
    bool PingMonitor::turnOnMonitoring(){
	if( targetIsSet ){
	    isMonitoring = true;
	    doPingForTarget();
	    return true;
	}
	return false;
    }

    // sets target to new target and turns on monitoring process if was off before
    // if new target is invalid, continues doing what we were doing before
	// if had been monitoring with previous target, keep doing that
	// if had not been monitoring, remain that way
    bool PingMonitor::switchMonitoringTarget( HostAndPort hp ){
	if( setTarget( hp )["ok"].boolean() ){
	    turnOnMonitoring();
	    return true;
	}
	return false;
    }

    void PingMonitor::clearMonitoringHistory(){
	//TODO
    }

    
    // Checking client connection commands

    // returns two fields: whether this process can make a client connection to
    // the target, and the connection message from the connect() command
    BSONObj PingMonitor::canConnect( HostAndPort hp ){
	BSONObjBuilder toReturn;
	DBClientConnection conn;
	string connInfo;
	bool isConnected;
	try{ 
	    isConnected = conn.connect( hp.toString(true) , connInfo );
	    toReturn.append("isConnected" , isConnected );
	    toReturn.append("connInfo" , connInfo);
	    toReturn.append("networkOk" , determineNetworkType( conn ) );
	}
	catch( DBException& e){ 
	    toReturn.append("isConnected" , false );
	    toReturn.append("connInfo" , connInfo);
	} 
	return toReturn.obj();
    }

    // returns the network type of the target; or null if unable to determine network type 
    // basically...
    // returns "sharded_cluster" if target is a mongos
    // returns "replica_set" if target is a primary in a replica set or a master in a master-slave relationshipt
    // returns NULL if target is a standalone mongod, secondary, or slave
    // returns NULL if unable to determine network type ( catches DBException )
    bool PingMonitor::determineNetworkType( DBClientConnection& conn ){
	BSONObj isMasterCmd = BSON( "isMaster" << 1 );
	BSONObj cmdReturned;
	try{
	    conn.runCommand( "admin" , isMasterCmd , cmdReturned );
	    if( cmdReturned["isMaster"].valuestrsafe() == false )
		return false;  
	    if( cmdReturned["msg"].trueValue() ){
		targetNetworkType = "sharded_cluster";
		return true;
	    }
	    if( cmdReturned["setName"].trueValue() ){
		targetNetworkType = "replica_set";
		return true;
	    }	
	}
	catch( DBException& e){
	    return false;
	}
	return false;
    }


//    BSONObj PingMonitor::monitorResults;
    BSONObj PingMonitor::getMonitorResults(){ 
	DBClientConnection conn;
	string connInfo;
	bool isConnected;
	isConnected = conn.connect( target , connInfo );
	BSONObj monitorResults;
	auto_ptr<DBClientCursor> cursor  = conn.query( "test.pingmonitor" , Query(BSONObj()).sort("_id",-1) );
	//how to connect to local db on self?
//	auto_ptr<DBClientCursor> cursor = conn.query( "test.pingmonitor" , BSONObj() );
	monitorResults = cursor->next();
	BSONObjBuilder show;
	BSONObjBuilder nodeShow;
	BSONObjBuilder edgeShow;
	for (BSONObj::iterator i = monitorResults.getObjectField("nodes").begin(); i.more(); ){
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
 
//    	show.append( "edges_full" , monitorResults.getObjectField("edges") ); 
//	show.append( "nodes_full" , monitorResults.getObjectField("nodes") );
	show.append( "edges" , edgeShow.obj() );
	show.append( "nodes" , nodeShow.obj() );

	show.append( "errors" , monitorResults.getObjectField("errors") );
	show.append( "warnings" , monitorResults.getObjectField("warnings") );
	return show.obj(); 
    }
    

    void PingMonitor::doPingForTarget(){
	if( isMonitoring == false )
	    return;
	//TODO
    	DBClientConnection conn;
	string connInfo;
	bool isConnected;
	isConnected = conn.connect( target , connInfo );
	//TODO error handling
	if( targetNetworkType == "replica_set" )
	    doPingForReplset( conn );
	else if( targetNetworkType == "sharded_cluster" )
	    doPingForCluster( conn );
    }

    void PingMonitor::doPingForReplset( DBClientConnection& conn ){
	//TODO
    }

    void PingMonitor::doPingForCluster( DBClientConnection& conn ){
/*
	struct addrinfo *info, *p;
	int gai_result;
	char hostname[1024];
	hostname[1023] = '\0';
	gethostname( hostname, 1023 );

	if( (gai_result = getaddrinfo( hostname, &info ) != 0 ) ){
	    fprintf( stderr, "getaddrinfo: %s\n", gai_strerror(gai_result) );
	    cout << "failed";
	}

	for( p = info; p!=NULL; p=p->ai_next){
	    printf("hostname: %s\n" , p->ai_canonname);
	}
*/

	BSONObjBuilder resultBuilder;
	resultBuilder.append( "target" , target.toString(true) );

	BSONObjBuilder nodesBuilder, edgesBuilder, idMapBuilder;
	BSONObj nodes, edges, idMap, errors, warnings;
	map<string, vector<string> > errorsBuilder, warningsBuilder;

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
	errors = convertToBSON( errorsBuilder );
	warnings = convertToBSON( warningsBuilder );

	resultBuilder.append("nodes" , nodes);   
	resultBuilder.append("edges" , edges);
	resultBuilder.append("idMap" , idMap);
	resultBuilder.append("errors" , errors);
	resultBuilder.append("warnings" , warnings); 
	resultBuilder.append("currentTime" , jsTime() );

	addNewNodes( conn , nodes );

	conn.insert( "test.pingmonitor" , resultBuilder.obj() );
//	monitorResults = resultBuilder.obj();	  
    }

    void PingMonitor::addNewNodes( DBClientConnection& conn , BSONObj& nodes ){
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

    void collect( DBClientConnection& conn , const string& db , BSONObjBuilder& newHost , BSONObj& cmd , BSONObj cmdReturned , const string& title ){
	try{
	    conn.runCommand( db , cmd , cmdReturned ); 
	    newHost.append( title , cmdReturned );
	}
	catch( DBException& e){
	    newHost.append( title , e.toString() );
	}
    }

    void collectClientInfo( string host , BSONObjBuilder& newHost , BSONObj& type , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	DBClientConnection conn;
	string connInfo;
	bool isConnected;
	try{ isConnected = conn.connect( host , connInfo );}
	catch(SocketException& e){ return; }
	if( isConnected == false ){ return; }

	BSONObj hostInfoCmd = BSON( "hostInfo" << 1 );
	BSONObj serverStatusCmd = BSON( "serverStatus" << 1 );
	BSONObj buildInfoCmd = BSON( "buildInfo" << 1 );
	BSONObj isMasterCmd = BSON( "isMaster" << 1 );
	BSONObj getShardVersionCmd = BSON( "getShardVersion" << 1 );
	BSONObj hostInfoReturned, serverStatusReturned, buildInfoReturned, isMasterReturned, getShardVersionReturned;

	//all nodes collect the following
	collect( conn , "admin" , newHost , hostInfoCmd, hostInfoReturned, "hostInfo" );
	collect( conn , "admin" , newHost , serverStatusCmd , serverStatusReturned, "serverStatus" );
	collect( conn , "admin" , newHost , getShardVersionCmd , getShardVersionReturned , "shardVersion" );

	// only mongod instances collect the following
	string mongod = "mongod";
	if( mongod.compare(type.getStringField("process")) == 0 )	
	    collect( conn , "admin" , newHost , buildInfoCmd , buildInfoReturned , "buildInfo" );

	string primary = "primary";
	string secondary = "secondary";
	// only shard mongod instances collect the following
	if ( primary.compare(type.getStringField("role")) == 0 || secondary.compare(type.getStringField("role")) == 0 )
	    collect( conn , "admin" , newHost , isMasterCmd , isMasterReturned , "isMaster" );
    }


    int PingMonitor::getShardServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	try{
	    auto_ptr<DBClientCursor> cursor = conn.query("config.shards" , BSONObj());
	    while( cursor->more()){
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
			newHost.append( "process" , "mongod" );
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
		    newHost.append( "process" , "mongod" );
		    newHost.append( "shardName" , shardName );
		    newHost.append( "key" , memberHosts + "_" + "" + "_" + "mongod" );
		    BSONObj type = BSON( "process" << "mongod" << "role" << "secondary" );
		    newHost.append( "type" , type );
		    collectClientInfo( currToken , newHost , type , errors , warnings );
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
		    newHost.append( "process" , "mongod" );
		    newHost.append( "key" , hostField + "_" + "" + "_" + "mongod" );
		    BSONObj type = BSON( "process" << "mongod" << "role" << "primary" );
		    collectClientInfo( p.getStringField("host") , newHost , type , errors , warnings );
		    char idString[100];
		    sprintf(idString , "%d" , id);
		    nodes.append( idString , newHost.obj() );
		} 
	    }
	}
	catch ( DBException& e ){};
	return index;
    }
    int PingMonitor::getMongosServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	try{
	    auto_ptr<DBClientCursor> cursor = conn.query("config.mongos" , BSONObj());
	    while( cursor->more()){
		BSONObj p = cursor->next();
		string hostField = p.getStringField("_id");
		int id = index;
		index++;
		BSONObjBuilder newHost;
		newHost.append( "hostName" , hostField );
		newHost.append( "machine" , "" );
		newHost.append( "process" , "mongos" );
		newHost.append( "key" , hostField + "_" + "" + "_" + "mongos" );
		BSONObj type = BSON( "process" << "mongos" << "role" << "mongos" );
		newHost.append( "type" , type );
		collectClientInfo( hostField , newHost , type , errors , warnings );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodes.append( idString , newHost.obj() );
	    }
	} catch( DBException& e) {} 
	return index;
    }

    int PingMonitor::getConfigServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	BSONObj cmdReturned;
	BSONObj getCmdLineOpts = BSON( "getCmdLineOpts" << 1 );
	try{
	    conn.runCommand( "admin" , getCmdLineOpts , cmdReturned );
	    string host = cmdReturned["parsed"]["configdb"].valuestrsafe();
	    int id = index;
	    index++;
	    BSONObjBuilder newHost;
	    newHost.append( "hostName" , host );
	    newHost.append( "machine" , "" );
	    newHost.append( "process" , "mongod");
	    newHost.append( "key" , host + "_" + "" + "_" + "mongod" );
	    BSONObj type = BSON( "process" << "mongod" << "role" << "config" );
	    newHost.append( "type" , type );
	    collectClientInfo( host , newHost , type , errors , warnings );
	    char idString[100];
	    sprintf(idString, "%d", id);
	    nodes.append( idString, newHost.obj() ); 
	} catch( DBException& e ){}
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
	    string srcHostName = srcObj.getStringField("hostName"); 
	    BSONObjBuilder srcEdges;
	    for( set<string>::iterator tgtId = tgtNodeIds.begin(); tgtId!=tgtNodeIds.end(); ++tgtId ){
		BSONElement tgtElem = nodes[ *tgtId ];
		BSONObj tgtObj = tgtElem.embeddedObject();
		string tgtHostName = tgtObj.getStringField("hostName"); 
		BSONObjBuilder newEdge;
		DBClientConnection conn;
		string connInfo;
		bool clientIsConnected;
		try{ clientIsConnected = conn.connect( srcHostName , connInfo ); }
		catch ( DBException& e ){ continue; } //TODO: better exception handling
		if( clientIsConnected ){
		    try{
			BSONObj hostArray = BSON( "0" << tgtHostName );
			BSONObjBuilder pingCmd;
			pingCmd.append("ping" , 1 );
			pingCmd.appendArray("hosts" , hostArray);		
			BSONObj cmdReturned;	
			conn.runCommand( "admin" , pingCmd.obj() , cmdReturned );
			BSONObj edgeInfo = cmdReturned.getObjectField( tgtHostName );
			if( edgeInfo["isConnected"].boolean() == false ){
			    newEdge.append( "isConnected" , false );
			    newEdge.append( "errmsg" , edgeInfo["errmsg"].valuestrsafe() );
			}
			else{
			    newEdge.append( "isConnected" , true );
/*TODO: fix this*/  	    newEdge.append( "pingTimeMicrosecs" , edgeInfo["pingTimeMicrosecs"].valuestrsafe() );
			}
			newEdge.append( "bytesSent" , edgeInfo["bytesSent"]._numberLong() );
			newEdge.append( "bytesReceived" , edgeInfo["bytesReceived"]._numberLong() );
			newEdge.append( "incomingSocketExceptions" , edgeInfo["incomingSocketExceptions"]._numberLong() );
			newEdge.append( "outgoingSocketExceptions" , edgeInfo["outgoingSocketExceptions"]._numberLong() );
			srcEdges.append( tgtElem.fieldName() , newEdge.obj() );  
		    }
		    catch( DBException& e) {
			newEdge.append( "isConnected" , "false" );
			newEdge.append( "errmsg" , /*TODO*/" " );
		    } 
		}
		else{ }//should be caught by exception handler? 
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


    void PingMonitor::run() {
//	Client::initThread( name().c_str() );
	while ( ! inShutdown() ) {
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
    	    sleepsecs( pingInterval );
	}
    }

    void startPingBackgroundJob() {
	PingMonitor::initializeCharts();
	PingMonitor* pmt = new PingMonitor();
	pmt->go();
    }

} 
