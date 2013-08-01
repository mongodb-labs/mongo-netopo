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

    const string PingMonitor::replicaSet = "replicaSet";
    const string PingMonitor::shardedCluster = "shardedCluster";

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
	m["CONFIG_ON_SHARD"] = " is running both a config and shard server";
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
		edgestr += tgtObj["pingTimeMicros"].valuestrsafe();
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
	else
  
	if( networkType == replicaSet )
	    doPingForReplset();
	else if( networkType == shardedCluster )
	    doPingForCluster();
    }

    void PingMonitor::doPingForReplset(){

	cout << "[PingMonitor::run()] : " << "PingMonitor thread for " << target.toString() << "  awake" << endl;

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
	resultBuilder.appendDate("currTime" , curTimeMillis64() );

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
	calculateDeltas();
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
		cout << "[PingMonitor::getSetServers()] : " << e.toString() << endl;
		return;
	    } 
	    connPtr->done();
	}
	catch ( DBException& e ){
	    // unable to connect to target
	    // TODO: log this error somewhere
	    cout << "[PingMonitor::getSetServers()] : " << e.toString() << endl;
	    return; 
	}

	int id = 0;

	if( cmdReturned["hosts"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["hosts"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
		BSONElement be = *i;
		string curr = be.valuestrsafe();
		BSONObjBuilder newHost;
		if( curr.compare(cmdReturned["primary"].valuestrsafe()) == 0 )
		    putGenericInfo( newHost , curr , "" , "mongod" , "primary" , errorsBuilder , warningsBuilder );
		else
		    putGenericInfo( newHost , curr , "" , "mongod" , "secondary" , errorsBuilder , warningsBuilder );
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
    
	cout << "[PingMonitor::run()] : " << "PingMonitor thread for " << target.toString() << "  awake" << endl;

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
	resultBuilder.appendDate("currTime" , curTimeMillis64() );

    	addNewNodes( nodes );
	
	BSONObj result = resultBuilder.obj();
	bool written = writeMonitorData( result ); 
	if( written == false ){
	    // TODO: make error an errcode 
	    cout << "[PingMonitor::doPingForCluster()] : " << "Failed to write PingMonitor data to self" << endl;
	}
 
	calculateStats();
	calculateDeltas();
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

    void PingMonitor::putGenericInfo( BSONObjBuilder& newHost , const string& hostName , const string& machine , const string& process , const string& role , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){

	// append all generic information found in the config server about this node
	newHost.append( "ipaddr" , "testing");
	newHost.append( "hostName" , hostName );
	newHost.append( "machine" , machine );
	newHost.append( "key" , hostName + "_" + machine + "_" + process );
	BSONObj type = BSON( "process" << process << "role" << role );;	
	newHost.append( "type" , type );

	// check if we can make a client connection to this host
	// and if so, ask it for more information about itself
	// and mark it as available to us
	try{ 
	    ScopedDbConnection conn( hostName  , socketTimeout ); 
	    newHost.append("available", true );
	    collectClientInfo( hostName , newHost , type , errors , warnings );
	    conn.done();
	}
	catch( DBException& e ){
	    addError(  hostName + "_" + machine + "_" + process, e.toString() , errors );
	    newHost.append("available", false );
	}


    }

    int PingMonitor::getArbitersAndPassives( string& master , BSONObjBuilder& nodesBuilder , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	BSONObj cmdReturned;
	try{ 
	    connPtr.reset( new ScopedDbConnection( master , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    try{
		conn->runCommand( "admin" , BSON("isMaster"<<1) , cmdReturned );	
	    }
	    catch ( DBException& e ){
		// unable to run isMaster command on target
		// TODO: log this error somewhere
		cout <<  "[" << __func__ << "] : "  << e.toString() << endl;
		return index;
	    } 
	    connPtr->done();
	}
	catch ( DBException& e ){
	    // unable to connect to target
	    // TODO: log this error somewhere
	    cout << "[" << __func__ << "] : "  << e.toString() << endl;
	    return index; 
	}
	
	if( cmdReturned["passives"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["passives"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
		int id = index;
		index++;
		BSONElement be = *i;
		string curr = be.valuestrsafe();
		BSONObjBuilder newHost;
		putGenericInfo( newHost , curr , "" , "mongod" , "passive" , errors , warnings );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodesBuilder.append( idString , newHost.obj() );
	    }
	}	

	if( cmdReturned["arbiters"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["arbiters"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
	    	int id = index;
		index++;
		BSONElement be = *i;
		string curr = be.valuestrsafe();
		BSONObjBuilder newHost;
		putGenericInfo( newHost , curr , "" , "mongod" , "arbiter" , errors , warnings );
		char idString[100];
		sprintf(idString , "%d" , id);
		nodesBuilder.append( idString , newHost.obj() );
	    }
	}

	return index;
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
			if( count == 0 ){
			    putGenericInfo( newHost , currToken , "" , "mongod" , "primary" , errors , warnings );
			    index = getArbitersAndPassives( currToken , nodes , index ,  errors , warnings);
			   // getPassives( currToken , nodes , errors , warnings );
			}
			else
			    putGenericInfo( newHost , currToken , "" , "mongod" , "secondary" , errors , warnings );
			newHost.append( "shardName" , shardName );
			char idString[100];
			sprintf(idString , "%d" , id);
			nodes.append( idString , newHost.obj() );
			memberHosts.erase(0, currPos + delimiter.length());
			count++; 
		    }			
		    int id = index;
		    index++;
		    BSONObjBuilder newHost;
		    putGenericInfo( newHost , memberHosts , "" , "mongod" , "secondary" , errors , warnings );
		    newHost.append( "shardName" , shardName );
		    char idString[100];
		    sprintf(idString , "%d" , id);
		    nodes.append( idString , newHost.obj() );
		}
		else{
		    int id = index;
		    index++;
		    BSONObjBuilder newHost;
		    putGenericInfo( newHost , p.getStringField("host") , "" , "mongod" , "primary" , errors , warnings );
		    char idString[100];
		    sprintf(idString , "%d" , id);
		    nodes.append( idString , newHost.obj() );
		} 
	    }
	connPtr->done();
	}
	catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
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
		putGenericInfo( newHost, hostField, "", "mongos", "mongos", errors , warnings);
		char idString[100];
		sprintf(idString , "%d" , id);
		nodes.append( idString , newHost.obj() );
	    }
	connPtr->done();
	} catch( DBException& e) {
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
	return index;
    }

    int PingMonitor::getConfigServers( HostAndPort& target , BSONObjBuilder& nodes , int index , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	string delimiter = ",";
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    BSONObj cmdReturned;
	    conn->runCommand("admin" , BSON("getCmdLineOpts"<<1) , cmdReturned ); 

	    string hosts = cmdReturned["parsed"]["configdb"].valuestrsafe();

	    int count = 0;
	    size_t currPos = 0;
	    string currToken;	
	    while( (currPos = hosts.find(delimiter)) != string::npos ){
		currToken = hosts.substr(0 , currPos);
		int id = index;
		index++;
		BSONObjBuilder newHost;
		putGenericInfo( newHost , currToken , "" , "mongod" , "config" , errors, warnings);
		char idString[100];
		sprintf(idString , "%d" , id);
		nodes.append( idString , newHost.obj() );
		hosts.erase(0, currPos + delimiter.length());
		count++; 
	    }			

	    int id = index;
	    index++;
	    BSONObjBuilder newHost;
	    putGenericInfo( newHost, hosts, "", "mongod" , "config" , errors, warnings);
	    char idString[100];
	    sprintf(idString , "%d" , id);
	    nodes.append( idString , newHost.obj() );

	    connPtr->done();
	} 
	catch( DBException& e) {
	    cout << "[PingMonitor::getShardServers()] : " << e.toString() << endl;
	}
	return index;
    }

    void PingMonitor::buildGraph( BSONObj& nodes , BSONObjBuilder& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){

	// create a map of <host:port> strings to < availability , id >
	map<string , pair<bool, string > > availableNodes;
	for( BSONObj::iterator n = nodes.begin(); n.more(); ){
	    BSONElement nElem = n.next();
	    BSONObj nObj = nElem.embeddedObject();
	    pair<bool, string > p ( nObj.getBoolField("available") , nElem.fieldName() );
	    availableNodes[ nObj.getStringField("hostName") ] = p;	    
	}

	for (map<string, pair<bool,string> >::iterator src = availableNodes.begin(); src!=availableNodes.end(); ++src ){
	    // only continue if we have been able to make a client connection to this instance (src)
	    if( src->second.first == true){
		string srcHostName = src->first;
		BSONObjBuilder srcEdges;
		for( map<string, pair<bool,string> >::iterator tgt = availableNodes.begin(); tgt!=availableNodes.end(); ++tgt){
		    // only continue if we have been able to make a client connection to this instance (tgt)
		    if( tgt->second.first == true ){	
			string tgtHostName = tgt->first;
			BSONObjBuilder newEdge;
			try{ 
			    ScopedDbConnection conn( srcHostName , socketTimeout ); 

			    // create deep ping command in the form { ping : 1 , hosts : [ ] }
			    BSONObjBuilder pingCmdBuilder;
			    pingCmdBuilder.append( "ping" , 1 );
			    BSONObj hostObj = BSON( "0" << tgtHostName );
			    pingCmdBuilder.appendArray( "hosts" , hostObj );
			    BSONObj pingCmd = pingCmdBuilder.obj();
			    BSONObj cmdReturned;
			    // run deep ping
			    conn->runCommand( "admin" , pingCmd , cmdReturned );	

			    // if the source was unable to ping the target, mark the target as unavailable
			    // and add an error or warning depending on the requirement level of the edge
			    if( cmdReturned["exceptionInfo"].trueValue() ){
				tgt->second.first = false;
				if( isReqConn( nodes.getObjectField( tgt->second.second ).getObjectField("type")["role"].valuestrsafe() , nodes.getObjectField( tgt->second.second ).getObjectField("type")["role"].valuestrsafe() ) )
				    addError( nodes.getObjectField( tgt->second.second )["key"].valuestrsafe() , cmdReturned["exceptionInfo"].valuestrsafe() , errors );
				if( isRecConn( nodes.getObjectField( tgt->second.second ).getObjectField("type")["role"].valuestrsafe() , nodes.getObjectField( tgt->second.second ).getObjectField("type")["role"].valuestrsafe() ) )
				    addWarning( nodes.getObjectField( tgt->second.second )["key"].valuestrsafe() , cmdReturned["exceptionInfo"].valuestrsafe() , warnings );
			    }

			    // append the target node's id with the result of the ping command to it
			    srcEdges.append( tgt->second.second , cmdReturned.getObjectField( tgtHostName ) );
			    conn.done();
			}
			catch( DBException& e ){
			    src->second.first = false;
			    cout << "[PingMonitor::buildGraph()] : " << e.toString() << endl;
			    string srcRole = nodes.getObjectField( src->second.second ).getObjectField("type")["role"].valuestrsafe();
			    string tgtRole =  nodes.getObjectField( tgt->second.second ).getObjectField("type")["role"].valuestrsafe();
			    if( isReqConn( srcRole , tgtRole ) )
				addError( nodes.getObjectField( tgt->second.second )["key"].valuestrsafe() , e.toString() , errors );
			    if( isRecConn( srcRole , tgtRole ) ) 
				addWarning( nodes.getObjectField( tgt->second.second )["key"].valuestrsafe() , e.toString() , warnings );
			}
		    }
		}
		// append the source node's id with all of its outgoing edges
		edges.append( src->second.second , srcEdges.obj() );
	    }
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

	string config = "config";
	vector<string> vec;
	for(BSONObj::iterator i = nodes.begin(); i.more(); ){
	    BSONElement iElem = i.next();
	    BSONObj iObj = iElem.embeddedObject();
	    if( config.compare(iObj.getObjectField("type")["role"].valuestrsafe()) == 0 ){
		vec.push_back( iObj["key"].valuestrsafe() );
	    }
	}	

	for(BSONObj::iterator i = nodes.begin(); i.more(); ){
	    BSONElement iElem = i.next();
	    BSONObj iObj = iElem.embeddedObject();
	    if( find( vec.begin(), vec.end(),  iObj["key"].valuestrsafe() ) != vec.end() ){
		addWarning( iObj["key"].valuestrsafe() , iObj["hostName"].valuestrsafe() + ERRCODES["CONFIG_ON_SHARD"] , warnings );
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
		map< int , map< string , map<string, long long> > > pingTime; // only used for intermediate steps;
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
		/*
		map< string , map<string, long long> > totalOutSocketExceptions;
		map< string , map<string, long long> > totalInSocketExceptions; 
		map< string , map<string, long long> > totalBytesSent;
		map< string , map<string, long long> > totalBytesRecd;
		*/

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
			    /*
			    totalOutSocketExceptions[ *src ][ *tgt ] = 0;
			    totalInSocketExceptions[ *src ][ *tgt ] = 0;
			    totalBytesSent[ *src ][ *tgt ] = 0;
			    totalBytesRecd[ *src ][ *tgt ] = 0;
			    */
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
				if( edge["pingTimeMicros"].trueValue() )
				    pingTime[ count ][ *src ][ *tgt ] = edge["pingTimeMicros"].numberLong();	
				else
				    pingTime[ count ][ *src ][ *tgt ] = 0;

				// socket exceptions and bytes sent/recd
				/*
				SocketException::Type i = SocketException::CLOSED;
				SocketException::Type end = SocketException::CONNECT_ERROR;
				while( i < end ){
				    targetInfo.append( SocketException::_getStringType(i) , SocketException::numExceptions( i , target ));
				    i = static_cast<SocketException::Type>( static_cast<int>(i) + 1 );
				}
				totalOutSocketExceptions[ *src ][ *tgt ] += edge["outgoingSocketExceptions"].numberLong();
				totalInSocketExceptions[ *src ][ *tgt ] += edge["incomingSocketExceptions"].numberLong();
				totalBytesSent[ *src ][ *tgt ] += edge["bytesSent"].numberLong();
				totalBytesRecd[ *src ][ *tgt ] += edge["bytesRecd"].numberLong();
				*/
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
		for( vector<string>::iterator src=allNodesList.begin(); src!=allNodesList.end(); ++src ){
		    for( vector<string>::iterator tgt=allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			if( (*src).compare( *tgt ) != 0 ){
			    BSONObjBuilder edge;
			    edge.append( "source" , *src );
			    edge.append( "target" , *tgt );
			    edge.append( "numPingAttempts" , numPingAttempts[ *src ][ *tgt ] );
			    edge.append( "numSuccessful" , numSuccessful[ *src ][ *tgt ] );
			    edge.append( "numFailed" , numFailed[ *src ][ *tgt ] );
			    edge.append( "percentConnected" , percentConnected[ *src ][ *tgt ] ); 
			    edge.append( "maxPingTime" , maxPingTime[ *src ][ *tgt ] );
			    edge.append( "minPingTime" , minPingTime[ *src ][ *tgt ] );
			    edge.append( "avgPingTime" , avgPingTime[ *src ][ *tgt ] );
			    edge.append( "pingTimeStdDev" , pingTimeStdDev[ *src ][ *tgt ] ); 
			    /*
			    edge.append( "totalOutSocketExceptions" , totalOutSocketExceptions[ *src ][ *tgt ] );
			    edge.append( "totalInSocketExceptions" , totalInSocketExceptions[ *src ][ *tgt ] ); 
			    edge.append( "totalBytesSent" , totalBytesSent[ *src ][ *tgt ] );
			    edge.append( "totalBytesRecd" , totalBytesRecd[ *src ][ *tgt ] );
			    */	
			    BSONObjBuilder idBuilder;
			    idBuilder.append( "source" , *src );
			    idBuilder.append( "target" , *tgt );			    
			    conn->update( statsLocation , idBuilder.obj() , edge.obj() , true); 
			}
		    }
		}
	    }
	    connPtr->done(); 
	}
	catch( DBException& e ){
	    cout << "[PingMonitor::calculateStats()] : " << e.toString() << endl;
	}
    }

    bool hasNotice( vector<BSONElement>& v , string notice ){
	for( vector<BSONElement>::iterator i = v.begin(); i!=v.end(); ++i){
	    if( notice.compare(i->String()) == 0 )
		return true;
	} 
	return false;
    }

    void PingMonitor::calculateDeltas(){
	scoped_ptr<ScopedDbConnection> connPtr;
	auto_ptr<DBClientCursor> cursor;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() ) ); 
	    ScopedDbConnection& conn = *connPtr;
	    cursor = conn->query( graphsLocation , Query(BSONObj()) );
	    if( cursor->more() == false ){
		// no graphs data to read from
		// TODO: do we need to log this? depends on how calculateDeltas is called
		// ERRCODES["NO_DATA"]
		connPtr->done();
		return;
	    }
	    else{
		int count = 0;
		BSONObj prev = BSONObj();
		BSONObj prevErrors = BSONObj();
		BSONObj prevWarnings = BSONObj();
		BSONObj prevIdMap = BSONObj();
		BSONObj prevNodes = BSONObj();

		while( cursor->more() ){
		    BSONObj curr = cursor->nextSafe();
		    BSONObj currErrors = curr.getObjectField("errors");
		    BSONObj currWarnings = curr.getObjectField("warnings");
		    BSONObj currIdMap = curr.getObjectField("idMap");
		    BSONObj currNodes = curr.getObjectField("nodes");
		    
//		    if( count > 0 ){
			BSONArrayBuilder newErrors;
			BSONArrayBuilder newWarnings;
			BSONArrayBuilder newNodes;
			BSONArrayBuilder removedErrors;
			BSONArrayBuilder removedWarnings;
			BSONArrayBuilder removedNodes;
			BSONArrayBuilder flags;

			// check for new nodes
			// and check for role change
			for( BSONObj::iterator i = currIdMap.begin(); i.more(); ){
			    string currKey = i.next().fieldName();
			    if( prevIdMap[ currKey ].eoo() == true )
				newNodes.append( currKey );
			    else{
				string prevRole = prevNodes.getObjectField( prevIdMap[ currKey ].valuestrsafe() ).getObjectField("type")["role"];	
				string currRole = currNodes.getObjectField( currIdMap[ currKey ].valuestrsafe() ).getObjectField("type")["role"];	
				if( currRole.compare( prevRole ) != 0 ){
				    string newFlag = "Host with key " + currKey + " changed roles from " + prevRole + " to " + currRole; 
				    flags.append( newFlag );	
				}
			    }
			}

			// check for removed nodes
			for( BSONObj::iterator i = prevIdMap.begin(); i.more(); ){
			    string prevKey = i.next().fieldName();
			    if( currIdMap[ prevKey ].eoo() == true )
				removedNodes.append( prevKey );
			}

			// check for new errors
			for( BSONObj::iterator i = currErrors.begin(); i.more(); ){
			    BSONElement currHost = i.next();
			    vector<BSONElement> currHostErrors = currHost.Array(); 
			    // if previous snapshot has no errors for this host, note all new errors 
			    if( prevErrors[ currHost.fieldName() ].trueValue() == false )
				for( vector<BSONElement>::iterator i = currHostErrors.begin(); i!=currHostErrors.end(); ++i){
				    newErrors.append( *i );	
			    }
			    // previous snapshot has errors for this host, but might be different from current ones
			    else{
				vector<BSONElement> prevHostErrors = prevErrors[ currHost.fieldName() ].Array();
				for( vector<BSONElement>::iterator i = currHostErrors.begin(); i!=currHostErrors.end(); ++i){
				    if( find( prevHostErrors.begin(), prevHostErrors.end(), *i ) == prevHostErrors.end() )
					newErrors.append( *i );	
				}
			    }
			}

			// check for new warnings
			for( BSONObj::iterator i = currWarnings.begin(); i.more(); ){
			    BSONElement currHost = i.next();
			    vector<BSONElement> currHostWarnings = currHost.Array(); 
			    // if previous snapshot has no warnings for this host, note all new warnings 
			    if( prevWarnings[ currHost.fieldName() ].trueValue() == false )
				for( vector<BSONElement>::iterator i = currHostWarnings.begin(); i!=currHostWarnings.end(); ++i){
				    newWarnings.append( *i );	
			    }
			    // previous snapshot has warnings for this host, but might be different from current ones
			    else{
				vector<BSONElement> prevHostWarnings = prevWarnings[ currHost.fieldName() ].Array();
				for( vector<BSONElement>::iterator i = currHostWarnings.begin(); i!=currHostWarnings.end(); ++i){
				    if( find( prevHostWarnings.begin(), prevHostWarnings.end(), *i ) == prevHostWarnings.end() )
					newWarnings.append( *i );	
				}
			    }
			}

			// check for removed errors
			for( BSONObj::iterator i = prevErrors.begin(); i.more(); ){
			    BSONElement prevHost = i.next();
			    vector<BSONElement> prevHostErrors = prevHost.Array(); 
			    // if current snapshot has no errors for this host, note all removed errors 
			    if( currErrors[ prevHost.fieldName() ].trueValue() == false )
				for( vector<BSONElement>::iterator i = prevHostErrors.begin(); i!=prevHostErrors.end(); ++i){
				    removedErrors.append( *i );	
			    }
			    // current snapshot has errors for this host, but might be different from prevent ones
			    else{
				vector<BSONElement> currHostErrors = currErrors[ prevHost.fieldName() ].Array();
				for( vector<BSONElement>::iterator i = prevHostErrors.begin(); i!=prevHostErrors.end(); ++i){
				    if( find( currHostErrors.begin(), currHostErrors.end(), *i ) == currHostErrors.end() )
					removedErrors.append( *i );	
				}
			    }
			}

			// check for removed warnings
			for( BSONObj::iterator i = prevWarnings.begin(); i.more(); ){
			    BSONElement prevHost = i.next();
			    vector<BSONElement> prevHostWarnings = prevHost.Array(); 
			    // if current snapshot has no warnings for this host, note all removed warnings 
			    if( currWarnings[ prevHost.fieldName() ].trueValue() == false )
				for( vector<BSONElement>::iterator i = prevHostWarnings.begin(); i!=prevHostWarnings.end(); ++i){
				    removedWarnings.append( *i );	
			    }
			    // current snapshot has warnings for this host, but might be different from prevent ones
			    else{
				vector<BSONElement> currHostWarnings = currWarnings[ prevHost.fieldName() ].Array();
				for( vector<BSONElement>::iterator i = prevHostWarnings.begin(); i!=prevHostWarnings.end(); ++i){
				    if( find( currHostWarnings.begin(), currHostWarnings.end(), *i ) == currHostWarnings.end() )
					removedWarnings.append( *i );	
				}
			    }
			}



			// save all deltas to the database

			BSONObjBuilder idBuilder;
			idBuilder.append( "currTime" , curr["currTime"].date() );
			idBuilder.append( "prevTime" , prev["currTime"].date() );

			BSONObjBuilder deltasBuilder;
			deltasBuilder.append( "currTime" , curr["currTime"].date() );
			deltasBuilder.append( "prevTime" , prev["currTime"].date() );
			deltasBuilder.append( "newNodes" , newNodes.arr() );
			deltasBuilder.append( "newErrors" , newErrors.arr() );
			deltasBuilder.append( "newWarnings" , newWarnings.arr() ); 
			deltasBuilder.append( "removedNodes" , removedNodes.arr() );
			deltasBuilder.append( "removedErrors" , removedErrors.arr() );
			deltasBuilder.append( "removedWarnings" , removedWarnings.arr() ); 
			conn->update( deltasLocation , idBuilder.obj() , deltasBuilder.obj() , true); 
//		    }
		    prev = curr;
		    prevErrors = currErrors;
		    prevWarnings = currWarnings;
		    prevIdMap = currIdMap;
		    count++;
		}
	    }
	   connPtr->done();
	} catch( DBException& e ){
	    cout << "[PingMonitor::calculateDeltas()] : " << e.toString() << endl;
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
