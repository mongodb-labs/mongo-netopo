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
#include "mongo/util/net/sock.h";

#include <boost/tokenizer.hpp>
#include <unistd.h>
#include <sstream>
#include <istream>
#include <climits>

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
	toReturn.append( "lastPingNetworkMicros" , lastPingNetworkMicros );
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
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    conn->dropCollection( writeLocation + SNAPSHOTS ); 
	    conn->dropCollection( writeLocation + NODESLIST ); 
	    conn->dropCollection( writeLocation + STATS ); 
	    conn->dropCollection( writeLocation + DELTAS ); 
	}
	catch( DBException& e){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
	if(connPtr!=0)connPtr->done();
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
	    if(connPtr!=0)connPtr->done(); 
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

	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr; 
	    conn->insert( writeLocation + SNAPSHOTS , snapshot.obj() );
	    numPings++;
	} catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
	if(connPtr!=0)connPtr->done();
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

	diagnose( nodes , edges , errorsBuilder , warningsBuilder );
	BSONObj errors = convertToBSON( errorsBuilder );
	BSONObj warnings = convertToBSON( warningsBuilder );

	calculateDeltas();
	calculateStats();

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
                   auto_ptr<DBClientCursor> cursor( conn->query( writeLocation , QUERY("key" <<  nElem.fieldName() ) ) );
                   if( !cursor->more() ){
                       // new node has been added
                       BSONObjBuilder thisNode;
                       thisNode.append( "key" , nElem.fieldName() );
                       conn->update( writeLocation + NODESLIST, BSON("key"<<nElem.fieldName()) , thisNode.obj() , true );
                   }
               }
           }
       }
       catch( DBException& e ){
           cout << "[" << __func__ << "] : " << e.toString() << endl;
       }
       if(connPtr!=0)connPtr->done();
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
	BSONObj cmdReturned;
	scoped_ptr<ScopedDbConnection> connPtr;
	try{ 
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    conn->runCommand( "admin" , BSON("isMaster"<<1) , cmdReturned );	
	}
	catch ( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	    //TODO: handle case that we can't get any information from the target
	    return; 
	}
	if(connPtr!=0)connPtr->done();

	if( cmdReturned["hosts"].trueValue() ){
	    vector<BSONElement> hosts = cmdReturned["hosts"].Array();
	    for( vector<BSONElement>::iterator i = hosts.begin(); i!=hosts.end(); ++i){
		BSONElement be = *i;
		const string curr = be.valuestrsafe();
		if( curr.compare(cmdReturned["primary"].valuestrsafe()) == 0 ){
		    addNode( nodes , curr , "mongod" , "primary" , errors , warnings );
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
	    auto_ptr<DBClientCursor> cursor( conn->query( "config.shards" , BSONObj() ) );
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
			if( count == 0 )
			    addNode( nodes , currToken , MONGOD , PRIMARY , errors , warnings , shardName );
			else
			    addNode( nodes , currToken , MONGOD , SECONDARY , errors , warnings , shardName );
			memberHosts.erase(0, currPos + delimiter.length());
			count++; 
		    }			
		    addNode( nodes , memberHosts , MONGOD , SECONDARY , errors , warnings , shardName );
		}
		else
		    addNode( nodes , p.getStringField("host") , MONGOD , PRIMARY , errors , warnings );
	    }
	}
	catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
	if(connPtr!=0)connPtr->done();
    }

    void PingMonitor::getArbitersAndPassives( const string& master , BSONObjBuilder& nodes , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	BSONObj cmdReturned;
	scoped_ptr<ScopedDbConnection> connPtr;
	try{ 
	    connPtr.reset( new ScopedDbConnection( master , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    conn->runCommand( "admin" , BSON("isMaster"<<1) , cmdReturned );	
	}
	catch ( DBException& e ){
	    cout << "[" << __func__ << "] : "  << e.toString() << endl;
	}
	if(connPtr!=0)connPtr->done();

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
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    scoped_ptr<DBClientCursor> cursor( conn->query( "config.mongos" , BSONObj() ) ) ;
	    while( cursor->more()){
		BSONObj p = cursor->nextSafe();
		addNode( nodes , p.getStringField("_id") , "mongos" , "mongos" , errors , warnings );
	    }
	} catch( DBException& e) {
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
	if(connPtr!=0)connPtr->done();
    }

    void PingMonitor::getConfigServers( HostAndPort& target , BSONObjBuilder& nodes , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	BSONObj cmdReturned;
	scoped_ptr<ScopedDbConnection> connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( target.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;
	    conn->runCommand("admin" , BSON("getCmdLineOpts"<<1) , cmdReturned ); 
	}
	catch( DBException& e) {
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	    if(connPtr!=0)connPtr->done();
	    return;
	}
	if(connPtr!=0)connPtr->done();

	string hosts = cmdReturned["parsed"]["configdb"].valuestrsafe();
	size_t currPos = 0;
	string delimiter = ",";
	while( (currPos = hosts.find(delimiter)) != string::npos ){
	    addNode( nodes , hosts.substr(0 , currPos) , "mongod" , "config" , errors , warnings );
	    hosts.erase(0, currPos + delimiter.length());
	}			
	addNode( nodes , hosts , "mongod" , "config" , errors , warnings );
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
	scoped_ptr< ScopedDbConnection > connPtr;	
	try{ 
	    connPtr.reset( new ScopedDbConnection( hostName , socketTimeout ) );
	    getArbitersAndPassives( hostName , nodes , errors , warnings );
	    bool stillConnected = collectClientInfo( key , hp , newHost , process , role , errors , warnings );
	    /* TODO
	    string machine =  "";
	    newHost.append( "machine" , machine ); */
	    if( stillConnected )
		newHost.append( "reachable" , true );
	}
	catch( DBException& e ){
	    newHost.append("reachable", false );
	    // TODO: make the key include the machine?
	    // Could be difficult if machine info only available by asking node about itself
	    addAlert(  key ,  e.toString() , errors );
	}
	if(connPtr!=0)connPtr->done();

	nodes.append( key , newHost.obj() ); 
    }

    bool PingMonitor::collectClientInfo( const string& key , const HostAndPort& hp , BSONObjBuilder& newHost , const string& process , const string& role , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){
	scoped_ptr< ScopedDbConnection > connPtr;
	try{
	    connPtr.reset( new ScopedDbConnection( hp.toString() , socketTimeout ) );
	    ScopedDbConnection& conn = *connPtr;

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
	}
	catch( DBException& e){
	    addAlert( key , e.toString() , errors );
	    if(connPtr!=0)connPtr->done();
	    return false;
	}
	if(connPtr!=0)connPtr->done();
	return true;
    }

    BSONObj createDeepPingCmd( vector<string>& reachableNodes , BSONObj& nodes ){
	BSONArrayBuilder targetArrB;
	for( vector<string>::iterator it = reachableNodes.begin(); it!=reachableNodes.end(); ++it)
	    targetArrB.append( nodes.getObjectField( *it )["hostName"].valuestrsafe() );
	BSONObjBuilder pingCmdBuilder;
	pingCmdBuilder.append( "ping" , 1 );
	pingCmdBuilder.append( "targets" , targetArrB.arr() );
	return pingCmdBuilder.obj();
    }

    void PingMonitor::buildGraph( BSONObj& nodes , BSONObjBuilder& edges , map<string, vector<string> >& errors , map<string, vector<string> >& warnings ){

	// create a vector of node keys for nodes that are marked as reachable
	vector< string > reachableNodes;
	for( BSONObj::iterator n = nodes.begin(); n.more(); ){
	    BSONElement nElem = n.next();
//	    if( nElem.embeddedObject().getBoolField("reachable") )
		reachableNodes.push_back( nElem.fieldName() );
	}

	for(vector<string>::iterator src = reachableNodes.begin(); src!=reachableNodes.end(); ++src ){
	    BSONObjBuilder srcEdges;

	    // create deep ping command
	    BSONObj pingCmd = createDeepPingCmd( reachableNodes , nodes ); 

	    // run deep ping command on all currently reachable nodes
	    BSONObj cmdReturned;
	    scoped_ptr< ScopedDbConnection> connPtr;
	    try{ 
		connPtr.reset( new ScopedDbConnection( nodes.getObjectField( *src )["hostName"].valuestrsafe() , socketTimeout ) );
		ScopedDbConnection& conn = *connPtr; 
	    	conn->runCommand( "admin" , pingCmd , cmdReturned );	
	    }
	    catch( DBException& e ){
		if(connPtr!=0)connPtr->done();
		continue;
	    }
	    if(connPtr!=0)connPtr->done();

	    // process results of deep ping command
	    vector<BSONElement> tgtResults = cmdReturned.getField("targets").Array();
	    for( vector<BSONElement>::iterator it = tgtResults.begin(); it!=tgtResults.end(); ++it){
		BSONObj tgtInfo = it->embeddedObject();
		if( tgtInfo["pingTimeMicros"].trueValue() == false && tgtInfo["exceptionInfo"].trueValue() == false){
		    // TODO: not a new enough version, add a warning?
		    // TODO: check earlier on if new enough version? Maybe send a deep ping to self
		    // the cmdReturned field will simply have "ok : 1"
		    // (we only pinged ourselves)
		    continue;
		}
		else{
		    string tgtKey = getKey( nodes , tgtInfo["hostName"].valuestrsafe() );
		    srcEdges.append( tgtKey , tgtInfo );
		    if( tgtInfo["exceptionInfo"].trueValue() ){
			// the source was unable to ping the target
			// and add an error or warning depending on the requirement level of the edge
			if( isReqConn( nodes.getObjectField( *src )["role"].valuestrsafe() , nodes.getObjectField( tgtKey )["role"].valuestrsafe() ) )
			    addAlert( *src , tgtInfo["exceptionInfo"].valuestrsafe() , errors );
			if( isRecConn( nodes.getObjectField( *src )["role"].valuestrsafe() , nodes.getObjectField( tgtKey )["role"].valuestrsafe() ) )
			    addAlert( *src , tgtInfo["exceptionInfo"].valuestrsafe() , warnings );
		    }
		}
	    } 
	    // append the source node's id with all of its outgoing edges
	    edges.append( *src , srcEdges.obj() );
	}
    }

    string PingMonitor::getKey( BSONObj& nodes , string hostName ){
	for( BSONObj::iterator n = nodes.begin(); n.more(); ){
	    BSONElement nElem = n.next();
	    if( hostName.compare( nElem.embeddedObject()["hostName"].valuestrsafe() ) == 0 )
		return nElem.fieldName();
	}
	return ""; 
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
	// check if config server is running on a shard server
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

	// TODO: other configuration diagnostics here
    }


    void PingMonitor::calculateStats(){
	scoped_ptr<ScopedDbConnection> connPtr;
	auto_ptr<DBClientCursor> cursor;
	try{
	    connPtr.reset( new ScopedDbConnection( self.toString() ) ); 
	    ScopedDbConnection& conn = *connPtr;
	    cursor = conn->query( writeLocation + SNAPSHOTS , Query(BSONObj()) );
	    if( cursor->more() == false ){
		if(connPtr!=0)connPtr->done();
		return;
	    }
	    else{
		
		// get a list of all node keys as strings
		auto_ptr<DBClientCursor> nodesListCursor = conn->query( writeLocation + NODESLIST,  Query(BSONObj()) );
		vector< string > allNodesList;
		while( nodesListCursor->more() )
		    allNodesList.push_back( nodesListCursor->next()["key"].valuestrsafe() );

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
		    BSONObj currNodes = curr.getObjectField("nodes");
		    for( vector<string>::iterator src=allNodesList.begin(); src!=allNodesList.end(); ++src ){
			for( vector<string>::iterator tgt=allNodesList.begin(); tgt!=allNodesList.end(); ++tgt ){
			    // if edge exists in this snapshot
			    if( currEdges[ *src ].trueValue() != false && currEdges.getObjectField( *src )[ *tgt ].trueValue() != false){
				BSONObj edge = currEdges.getObjectField( *src ).getObjectField( *tgt );
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
	}
	catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
	if(connPtr!=0)connPtr->done(); 
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
	    int nToReturn = 2;
	    cursor = conn->query( writeLocation + SNAPSHOTS , Query(BSONObj()).sort("_id",-1) , nToReturn );
	    if( cursor->more() == false ){
		// no graphs data to read from
		// TODO: do we need to log this? depends on how calculateDeltas is called
		// ERRCODES["NO_DATA"]
		if(connPtr!=0)connPtr->done();
		return;
	    }
	    else{
		int count = 0;
		BSONObj prevErrors = BSONObj();
		BSONObj prevWarnings = BSONObj();
		BSONObj prevNodes = BSONObj();
		BSONObj prev = BSONObj();

		while( cursor->more() ){
		    BSONObj curr = cursor->nextSafe();
		    BSONObj currErrors = curr.getObjectField("errors");
		    BSONObj currWarnings = curr.getObjectField("warnings");
		    BSONObj currNodes = curr.getObjectField("nodes");
		    
		    if( count > 0 ){
			BSONArrayBuilder newErrors;
			BSONArrayBuilder newWarnings;
			BSONArrayBuilder newNodes;
			BSONArrayBuilder removedErrors;
			BSONArrayBuilder removedWarnings;
			BSONArrayBuilder removedNodes;
			BSONArrayBuilder flags;

			// check for new nodes
			// and check for role change
			for( BSONObj::iterator i = currNodes.begin(); i.more(); ){
			    string currKey = i.next().fieldName();
			    if( prevNodes[ currKey ].eoo() == true )
				newNodes.append( currKey );
			    else{
				// the node existed in the previous ping
				string prevRole = prevNodes.getObjectField( currKey )["role"].valuestrsafe();
				string currRole = currNodes.getObjectField( currKey )["role"].valuestrsafe();
				if( currRole.compare( prevRole ) != 0 ){
				    string newFlag = "Host with key " + currKey + " changed roles from " + prevRole + " to " + currRole; 
				    flags.append( newFlag );	
				}
			    }
			}

			// check for removed nodes
			for( BSONObj::iterator i = prevNodes.begin(); i.more(); ){
			    string prevKey = i.next().fieldName();
			    if( currNodes[ prevKey ].eoo() == true )
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
			deltasBuilder.append( "flags" , flags.arr() );
			conn->update( writeLocation + DELTAS , idBuilder.obj() , deltasBuilder.obj() , true); 
		    }
		    prev = curr;
		    prevErrors = currErrors;
		    prevWarnings = currWarnings;
		    prevNodes = currNodes;
		    count++;
		}
	    }
    	} catch( DBException& e ){
	    cout << "[" << __func__ << "] : " << e.toString() << endl;
	}
	if(connPtr!=0)connPtr->done();

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
	lastPingNetworkMicros = 0;
	while ( ! inShutdown() && alive ) {
	    /*
	    if( lockedForWriting() ) {
		// note: this is not perfect as you can go into fsync+lock between
		// this and actually doing the delete later
		cout << " locked for writing" << endl;
		continue;
	    }
	    */

	    Timer pingTimer;
	    doPingForTarget();
	    lastPingNetworkMicros = pingTimer.micros();
	    
    	    sleepsecs( interval );
	}
    }

} 
