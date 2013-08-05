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


    // string literals
    const string REPLICASET = "replicaSet";
    const string SHARDEDCLUSTER = "shardedCluster";

    const string PRIMARY = "primary";
    const string SECONDARY = "secondary";
    const string ARBITER = "arbiter";
    const string PASSIVE = "passive";
    const string CONFIG = "config";

    const string MONGOD = "mongod";
    const string MONGOS = "mongos";

    const string NODESLIST = "nodeslist";
    const string STATS = "stats";
    const string DELTAS = "deltas";
    const string SNAPSHOTS = "snapshots";

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
	    conn->dropCollection( writeLocation + SNAPSHOTS ); 
	    conn->dropCollection( writeLocation + NODESLIST ); 
	    conn->dropCollection( writeLocation + STATS ); 
	    conn->dropCollection( writeLocation + DELTAS ); 
	    conn.done();
	}
	catch( DBException& e){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
    }

    // Retrieve from ping monitor storage database
    BSONObj PingMonitor::getMonitorInfo(){ 
	BSONObjBuilder toReturn;
/*	BSONObj latestMonitorInfo;
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
	*/
	return toReturn.obj(); 
    }
    
    void PingMonitor::writeData( BSONObj& nodes , BSONObj& edges , BSONObj& errors , BSONObj&  warnings , long long currTime ){
	BSONObjBuilder snapshot;
	snapshot.append( "currTime" , currTime );
	snapshot.append( "nodes" , nodes );
	snapshot.append( "edges" , edges );
	snapshot.append( "errors" , errors );
	snapshot.append( "warnings" , warnings);

	try{
	    ScopedDbConnection conn( self.toString() , socketTimeout); 
	    conn->insert( writeLocation + SNAPSHOTS , snapshot.obj() );
	    conn.done();
	    numPings++;
	} catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
    }

    void PingMonitor::doPingForTarget(){
	if( on == false )
	    return;
   
	cout << "[" << __func__ << "] : " << "PingMonitor thread for " << target.toString() << "  awake" << endl;

	long long currTime = curTimeMillis64();

	BSONObjBuilder nodesBuilder, edgesBuilder; 
	map<string, vector<string> > errorsBuilder, warningsBuilder;

	if( networkType == REPLICASET )
	    getSetServers( target , nodesBuilder , errorsBuilder , warningsBuilder );
	else{ 
	    getShardServers( target , nodesBuilder , errorsBuilder , warningsBuilder );
	    getMongosServers( target , nodesBuilder , errorsBuilder , warningsBuilder );
	    getConfigServers( target , nodesBuilder , errorsBuilder , warningsBuilder );
	}
	BSONObj nodes = nodesBuilder.obj();

	updateNodesList( nodes );

	buildGraph( nodes , edgesBuilder , errorsBuilder , warningsBuilder );
	BSONObj edges = edgesBuilder.obj();

	// TODO: add network type check in diagnose()
	//diagnose( nodes , edges , errorsBuilder , warningsBuilder );
	BSONObj errors = convertToBSON( errorsBuilder );
	BSONObj warnings = convertToBSON( warningsBuilder );

	//calculateDeltas();

	writeData( nodes , edges , errors , warnings , currTime ); 
    }

    void PingMonitor::updateNodesList( BSONObj& nodes ){
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() , socketTimeout ) ); 
	    ScopedDbConnection& conn = *connPtr;
	    for( BSONObj::iterator i=nodes.begin(); i.more(); ){
		BSONElement nElem = i.next();
		if( nElem.isSimpleType() == false ){ 
    		    scoped_ptr<DBClientCursor> cursor( conn->query( writeLocation , QUERY("key" <<  nElem.fieldName() ) ) );
		    if( !cursor->more() ){
			// new node has been added
			BSONObjBuilder thisNode;
			thisNode.append( "key" , nElem.fieldName() );
			conn->update( writeLocation + NODESLIST, BSON("key"<<nElem.fieldName()) , thisNode.obj() , true );
		    }
		}
	    }	
	connPtr->done();	
	}
	catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
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

    void PingMonitor::getSetServers( HostAndPort& target , BSONObjBuilder& nodes , map< string , vector<string> >& errors , map< string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	BSONObj cmdReturned;
	try{ 
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    conn->runCommand( "admin" , BSON("isMaster"<<1) , cmdReturned );	
	    connPtr->done();
	}
	catch ( DBException& e ){
	    cout << "[PingMonitor::getSetServers()] : " << e.toString() << endl;
	    //TODO: handle case that we can't get any information from the target
	    return; 
	}
	if( cmdReturned["hosts"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["hosts"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
		BSONElement be = *i;
		const string curr = be.valuestrsafe();
		if( curr.compare(cmdReturned["primary"].valuestrsafe()) == 0 ){
		    addNode( nodes , curr , "mongod" , "primary" , errors , warnings );
		    getArbitersAndPassives( curr , nodes , errors , warnings );
		}
		else
		    addNode( nodes , curr , "mongod" , "secondary" , errors , warnings );
	    }
	}	
    }

    void PingMonitor::getShardServers( HostAndPort& target , BSONObjBuilder& nodes , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
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
		    while( (currPos = memberHosts.find(delimiter)) != string::npos ){
			const string currToken = memberHosts.substr(0 , currPos);
			if( count == 0 ){
			    getArbitersAndPassives( currToken , nodes ,  errors , warnings);
			    addNode( nodes , currToken , "mongod" , "primary" , errors , warnings , shardName ); }
			else
			    addNode( nodes , currToken , "mongod" , "secondary" , errors , warnings , shardName );
			memberHosts.erase(0, currPos + delimiter.length());
			count++; }			
		    addNode( nodes , memberHosts , "mongod" , "secondary" , errors , warnings );}
		else
		    addNode( nodes , p.getStringField("host") , "mongod" , "primary" , errors , warnings );}
	    connPtr->done(); }
	catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
    }

    void PingMonitor::getArbitersAndPassives( const string& master , BSONObjBuilder& nodes , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	BSONObj cmdReturned;
	try{ 
	    connPtr.reset( new ScopedDbConnection( master , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    conn->runCommand( "admin" , BSON("isMaster"<<1) , cmdReturned );	
	    connPtr->done();
	}
	catch ( DBException& e ){
	    cout << "[" << __func__ << "] : "  << e.toString() << endl;
	}
	if( cmdReturned["passives"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["passives"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i)
		addNode( nodes , (*i).valuestrsafe() , "mongod" , "passive" , errors , warnings );
	}	
	if( cmdReturned["arbiters"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["arbiters"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i)
		addNode( nodes , (*i).valuestrsafe() , "mongod" , "arbiter" , errors , warnings );
	}
    }

     void PingMonitor::getMongosServers( HostAndPort& target , BSONObjBuilder& nodes , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	auto_ptr<DBClientCursor> cursor;
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    scoped_ptr<DBClientCursor> cursor( conn->query( "config.mongos" , BSONObj() ) ) ;
	    while( cursor->more()){
		BSONObj p = cursor->nextSafe();
		addNode( nodes , p.getStringField("_id") , "mongos" , "mongos" , errors , warnings );
	    }
	    connPtr->done();
	} catch( DBException& e) {
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
    }

    void PingMonitor::getConfigServers( HostAndPort& target , BSONObjBuilder& nodes , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    BSONObj cmdReturned;
	    conn->runCommand("admin" , BSON("getCmdLineOpts"<<1) , cmdReturned ); 
	    string hosts = cmdReturned["parsed"]["configdb"].valuestrsafe();
	    size_t currPos = 0;
	    string currToken;	
	    string delimiter = ",";
	    while( (currPos = hosts.find(delimiter)) != string::npos ){
		addNode( nodes , hosts.substr(0 , currPos) , "mongod" , "config" , errors , warnings );
		hosts.erase(0, currPos + delimiter.length());
	    }			
	    addNode( nodes , hosts , "mongod" , "config" , errors , warnings );
	    connPtr->done();
	} 
	catch( DBException& e) {
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
    }

    void PingMonitor::addNode( BSONObjBuilder& nodes , const string& hostName , const string& process , const string& role , map< string , vector<string> >& errors , map<string , vector<string> >& warnings , const string& shardName ){

	HostAndPort hp;
	try{
	    hp = HostAndPort( hostName );
	}
	catch( std::exception e ){
	    //TODO: hostname is invalid
	    return;
	} 

	BSONObjBuilder newHost;
	string key = hostName + "_" + process;
	newHost.append( "hostName" , hostName );
	newHost.append( "process" , process );
	newHost.append( "role" , role );
	if( shardName.compare( "" ) != 0 )
	    newHost.append( "shardName" , shardName );
	// check if we can make a client connection to this host
	// and if so, mark it as reachable
	// and ask it for more information about itself
	try{ 
	    ScopedDbConnection conn( hostName  , socketTimeout ); 
	    bool stillConnected = collectClientInfo( key , hp , newHost , process , role , errors , warnings );
	    /* TODO
	    string machine =  "";
	    newHost.append( "machine" , machine ); */
	    if( stillConnected )
		newHost.append( "reachable" , true );
	    conn.done();
	}
	catch( DBException& e ){
	    newHost.append("reachable", false );
	    // TODO: make the key include the machine?
	    // Could be difficult if machine info only available by asking node about itself
	    addAlert(  key ,  e.toString() , errors );
	}
	nodes.append( key , newHost.obj() ); 
    }

    bool PingMonitor::collectClientInfo( const string& key , const HostAndPort& hp , BSONObjBuilder& newHost , const string& process , const string& role , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	try{
	    ScopedDbConnection conn( hp.toString() );

	    // all nodes collect the following
	    { BSONObj cmdReturned;
	    conn->runCommand( "admin" , BSON("hostInfo"<<1) , cmdReturned );
	    newHost.append( "hostInfo" , cmdReturned ); }

	    { BSONObj cmdReturned;
	    conn->runCommand( "admin" , BSON("serverStatus"<<1) , cmdReturned );
	    newHost.append( "serverStatus" , cmdReturned ); }

	    { BSONObj cmdReturned;
	    conn->runCommand( "admin" , BSON("shardVersion"<<1) , cmdReturned );
	    newHost.append( "shardVersion" , cmdReturned ); }

	    // only mongod instances collect the following
	    if( process.compare( MONGOD ) == 0 ){
		BSONObj cmdReturned;
		conn->runCommand( "admin" , BSON("buildInfo"<<1) , cmdReturned );
		newHost.append( "buildInfo" , cmdReturned ); }

	    /* The info in isMaster doesn't seem all that useful and is kind of redundant?
	    // only shard mongod instances collect the following
	    if( role.compare( PRIMARY ) == 0 || role.compare( SECONDARY ) == 0 ){
		BSONObj cmdReturned;
		conn->runCommand( "admin" , BSON("isMaster"<<1) , cmdReturned );
		newHost.append( "isMaster" , cmdReturned ); }
	    */
	    conn.done();
	}
	catch( DBException& e){
	    addAlert( key , e.toString() , errors );
	    return false;
	}
	return true;
    }

    BSONObj createDeepPingCmd( const string& tgt ){
	BSONObjBuilder pingCmdBuilder;
	pingCmdBuilder.append( "ping" , 1 );
	BSONObj hostObj = BSON( "0" << tgt );
	pingCmdBuilder.appendArray( "hosts" , hostObj );
	return pingCmdBuilder.obj();
    }

    void PingMonitor::buildGraph( BSONObj& nodes , BSONObjBuilder& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	// create a map of node keys to reachability
	map< string , bool > reachableNodes;
	for( BSONObj::iterator n = nodes.begin(); n.more(); ){
	    BSONElement nElem = n.next();
	    // TODO: might not be the most efficient thing... the problem is that hte last field is "currTime"
	    if( nElem.isSimpleType() == false )
		reachableNodes[ nElem.fieldName() ] = nElem.embeddedObject().getBoolField("reachable");	    
	}
	for (map<string, bool >::iterator src = reachableNodes.begin(); src!=reachableNodes.end(); ++src ){
	    // only continue if src is reachable
	    if( src->second == true){
		string srcKey = src->first;
		BSONObjBuilder srcEdges;
		for( map<string, bool >::iterator tgt = reachableNodes.begin(); tgt!=reachableNodes.end(); ++tgt){
		    // only continue if tgt is reachable
		    if( tgt->second == true ){	
			string tgtKey = tgt->first;
			BSONObjBuilder newEdge;
			try{ 
			    ScopedDbConnection conn( nodes.getObjectField( srcKey )["hostName"].valuestrsafe() , socketTimeout ); 
			    BSONObj cmdReturned;
			    BSONObj pingCmd = createDeepPingCmd( nodes.getObjectField( tgtKey )["hostName"].valuestrsafe() ); 
			    conn->runCommand( "admin" , pingCmd , cmdReturned );	
			    if( cmdReturned["pingTimeMicros"].trueValue() == false && cmdReturned["exceptionInfo"].trueValue() == false){
				//TODO: not a new enough version, add a warning?
				// the cmdReturned field will simply have "ok : 1"
				srcEdges.append( tgt->first , cmdReturned.getObjectField( nodes.getObjectField( tgtKey )["hostName"].valuestrsafe() ) );
				conn.done();
				continue;
			    }
			    // if the source was unable to ping the target, mark the target as unreachable
			    // and add an error or warning depending on the requirement level of the edge
			    if( cmdReturned["exceptionInfo"].trueValue() ){
				tgt->second = false;
				if( isReqConn( nodes.getObjectField( srcKey )["role"].valuestrsafe() , nodes.getObjectField( tgtKey )["role"].valuestrsafe() ) )
				    addAlert( srcKey , cmdReturned["exceptionInfo"].valuestrsafe() , errors );
				if( isRecConn( nodes.getObjectField( srcKey )["role"].valuestrsafe() , nodes.getObjectField( tgtKey )["role"].valuestrsafe() ) )
				    addAlert( srcKey , cmdReturned["exceptionInfo"].valuestrsafe() , warnings );
			    }
			    else{
				// add an edge with the target node's key and the result of the ping command
				srcEdges.append( tgt->first , cmdReturned.getObjectField( nodes.getObjectField( tgtKey )["hostName"].valuestrsafe() ) );
			    }
			    conn.done();
			}
			catch( DBException& e ){
			    src->second = false;
			    cout << "[" << __func__ << "] : " << e.toString() << endl;
			    //TODO: add an edge but have a way to tell that you weren't able to run it?			  
  			}
		    }
		}
		// append the source node's id with all of its outgoing edges
		edges.append( src->first , srcEdges.obj() );
	    }
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

    void PingMonitor::addAlert( const string& key , const string& alert , map<string, vector<string> >& loc ){
	vector<string> list = loc[ key ];
	vector<string>::iterator it;
	if( (it = find( list.begin(), list.end(), alert )) == list.end() )
	    list.push_back( alert );
	loc[ key ] = list;
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
		addAlert( iObj["key"].valuestrsafe() , iObj["hostName"].valuestrsafe() + ERRCODES["CONFIG_ON_SHARD"] , warnings );
	    }
	}
    }

/*
    void PingMonitor::calculateStats(){
	scoped_ptr<ScopedDbConnection> connPtr;
	auto_ptr<DBClientCursor> nodes;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() ) ); 
	    ScopedDbConnection& conn = *connPtr;
	    nodes = conn->query( graphsLocation , Query(BSONObj()) );
	    if( nodes->more() == false ){
		// no graphs data to read from
		// TODO: do we need to log this?
		// ERRCODES["NO_DATA"]
		connPtr->done();
		return;
	    }
	    else{
		
		// get a list of all node keys as strings
		auto_ptr<DBClientCursor> nodes = conn->query( writeLocation, Query(BSONObj()) );
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
			    BSONObjBuilder idBuilder;
			    idBuilder.append( "source" , *src );
			    idBuilder.append( "target" , *tgt );			    
			    conn->update( writeLocation + STATS , idBuilder.obj() , edge.obj() , true); 
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
*/


    bool hasNotice( vector<BSONElement>& v , string notice ){
	for( vector<BSONElement>::iterator i = v.begin(); i!=v.end(); ++i){
	    if( notice.compare(i->String()) == 0 )
		return true;
	} 
	return false;
    }

    void PingMonitor::calculateDeltas(){
/*	scoped_ptr<ScopedDbConnection> connPtr;
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
			conn->update( writeLocation + DELTAS , idBuilder.obj() , deltasBuilder.obj() , true); 
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
*/
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
    }

} 
