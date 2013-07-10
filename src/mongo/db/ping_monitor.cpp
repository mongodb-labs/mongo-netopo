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

namespace mongo {

    // PingMonitor

    boost::mutex PingMonitor::_mutex;

    BSONObj PingMonitor::monitorResults;

    BSONObj PingMonitor::getMonitorResults(){ 
	BSONObjBuilder show;
	BSONObjBuilder nodeShow;
//	BSONObjBuilder edgeShow;
	
	for (BSONObj::iterator i = monitorResults.getObjectField("nodes").begin(); i.more(); ){
	    BSONElement nodeElem = i.next();
	    BSONObj nodeObj = nodeElem.embeddedObject(); 
	    BSONObjBuilder nodeData;
	    nodeData.append( "hostName" , nodeObj["hostName"].valuestrsafe() );
	    nodeData.append( "process" , nodeObj["process"].valuestrsafe() );
	    nodeData.append( "role" , nodeObj.getObjectField("type").getStringField("role") );
	    nodeData.append( "uptimeMillis" , nodeObj.getObjectField("serverStatus")["uptimeMillis"]._numberLong() );
	    nodeShow.append( nodeElem.fieldName() , nodeData.obj() ); 
	}
	show.append( "edges" , monitorResults.getObjectField("edges") ); 
	show.append( "nodes" , nodeShow.obj() ); 
/*	for (BSONObj::iterator i = monitorResults.getObjectField("edges").begin(); i.more(); ){
	    BSONElement nodeElem = i.next();
//	    BSONObj nodeObj = nodeElem.embeddedObject();
	}*/	
	return show.obj(); 
    }
 
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
	resultBuilder.append("currentTime" , jsTime() );
	monitorResults = resultBuilder.obj();	  

    }

    void collectClientInfo( string host , BSONObjBuilder& newHost , BSONObj& type , BSONObjBuilder& errors , BSONObjBuilder& warnings ){
	DBClientConnection conn;
	string connInfo;
	bool isConnected;
	try{ isConnected = conn.connect( host , connInfo );}
	catch(SocketException& e){ return; }
	if( isConnected == false ){ return; }

	/*TODO: could push all these commands into a vector and simply loop over the vector*/
	BSONObj hostInfoCmd = BSON( "hostInfo" << 1 );
	BSONObj serverStatusCmd = BSON( "serverStatus" << 1 );
	BSONObj buildInfoCmd = BSON( "buildInfo" << 1 );
	BSONObj isMasterCmd = BSON( "isMaster" << 1 );
	BSONObj getShardVersionCmd = BSON( "getShardVersion" << 1 );
	BSONObj hostInfoReturned, serverStatusReturned, buildInfoReturned, isMasterReturned, getShardVersionReturned;

	//all nodes collect the following
	try{
	    conn.runCommand( "admin" , hostInfoCmd , hostInfoReturned ); 
	    newHost.append( "hostInfo" , hostInfoReturned );
	}
	catch( DBException& e){
	    newHost.append( "hostInfo" , e.toString() );
	}

	try{
	    conn.runCommand( "admin" , serverStatusCmd , serverStatusReturned );
	    newHost.append( "serverStatus" , serverStatusReturned );
	}
	catch ( DBException& e ){
	    newHost.append( "serverStatus" , e.toString() );
	} 

	try{
	    conn.runCommand( "admin" , getShardVersionCmd , getShardVersionReturned );
	    newHost.append( "shardVersion" , getShardVersionReturned );
	}	
	catch ( DBException& e ){
	    newHost.append( "shardVersion" , e.toString() );
	}

	// only mongod instances collect the following
	string mongod = "mongod";
	if( mongod.compare(type.getStringField("process")) == 0 ){	
	    try{
		conn.runCommand( "admin" , buildInfoCmd , buildInfoReturned );
		newHost.append( "buildInfo" , buildInfoReturned );
	    }
	    catch ( DBException& e ){
		newHost.append( "buildInfo" , e.toString() );
	    }
	}

	string primary = "primary";
	string secondary = "secondary";
	// only shard mongod instances collect the following
	if ( primary.compare(type.getStringField("role")) == 0 || secondary.compare(type.getStringField("role")) == 0 ){
	    try{
		conn.runCommand( "admin" , isMasterCmd , isMasterReturned );
		newHost.append( "isMaster" , isMasterReturned );
	    }
	    catch ( DBException& e) {
		newHost.append( "isMaster" , e.toString() );
	    }
	}
    }

    int PingMonitor::getShardServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings ){
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
			if( count == 0 ){
			    newHost.append("role" , "primary" );
			    type = BSON( "process" << "mongod" << "role" << "primary" );
			}
			else{
			    newHost.append("role" , "secondary" );
			    type = BSON( "process" << "mongod" << "role" << "secondary" );
			}	
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
		    newHost.append( "hostName" , currToken );
		    newHost.append( "machine" , "" );
		    newHost.append( "process" , "mongod" );
		    newHost.append( "shardName" , shardName );
		    newHost.append( "key" , currToken + "_" + "" + "_" + "mongod" );
		    BSONObj type = BSON( "process" << "mongod" << "role" << "secondary" );
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
    int PingMonitor::getMongosServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings ){
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
		collectClientInfo( hostField , newHost , type , errors , warnings );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodes.append( idString , newHost.obj() );
	    }
	} catch( DBException& e) {} 
	return index;
    }

    int PingMonitor::getConfigServers( DBClientConnection& conn , BSONObjBuilder& nodes , int index , BSONObjBuilder& errors , BSONObjBuilder& warnings ){
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
	    collectClientInfo( host , newHost , type , errors , warnings );
	    char idString[100];
	    sprintf(idString, "%d", id);
	    nodes.append( idString, newHost.obj() ); 
	} catch( DBException& e ){}
	return index;
    }

    void PingMonitor::buildGraph( BSONObj& nodes , BSONObjBuilder& edges , BSONObjBuilder& errors , BSONObjBuilder& warnings ){
	for (BSONObj::iterator srcNode = nodes.begin(); srcNode.more(); ){
	    BSONElement srcElem = srcNode.next();
	    BSONObj srcObj = srcElem.embeddedObject();
	    string srcHostName = srcObj.getStringField("hostName"); 
	    BSONObjBuilder srcEdges;
	    for( BSONObj::iterator tgtNode = nodes.begin(); tgtNode.more(); ){
		BSONElement tgtElem = tgtNode.next();
		BSONObj tgtObj = srcElem.embeddedObject();
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
			    newEdge.append( "pingTimeMicrosecs" , edgeInfo["pingTimeMicrosecs"].number() );
			}
			newEdge.append( "bytesSent" , edgeInfo["bytesSent"]._numberLong() );
			newEdge.append( "bytesReceived" , edgeInfo["bytesReceived"]._numberLong() );
			newEdge.append( "incomingSocketExceptions" , edgeInfo["incomingSocketExceptions"]._numberLong() );
			newEdge.append( "outgoingSocketExceptions" , edgeInfo["outgoingSocketExceptions"]._numberLong() );
			srcEdges.append( tgtElem.fieldName() , newEdge.obj() );  
		    }
		    catch( DBException& e) {
			newEdge.append( "isConnected" , "false" );
			newEdge.append( "errmsg" , " " );
		    } 
		}
		else{ }//should be caught by exception handler? 
	    }
	    edges.append( srcElem.fieldName() , srcEdges.obj() );
	}
    }

    void PingMonitor::buildIdMap( BSONObj& nodes , BSONObjBuilder& idMap ){

    }

    void PingMonitor::diagnose( BSONObj& nodes , BSONObj& edges , BSONObjBuilder& errors , BSONObjBuilder& warnings ){

    }

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
    	    sleepsecs( 15 );
	}
    }

    void startPingBackgroundJob() {
	PingMonitor* pmt = new PingMonitor();
	pmt->go();
    }

} 
