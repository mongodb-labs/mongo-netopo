var history = {};
history["snapshots"] = {};
history["allNodes"] = {};
history["stats"] = {};
history["deltas"] =  {};

/*stand-in names for strings used in output*/
var shards = "shards";
var mongos = "mongos";
var config = "config";
var primary = "primary";
var secondary = "secondary";
var connReqsMet =  "connReqsMet";
var req = "required";
var rec = "recommended";
var notReq = "notRequired";
 
var id = "_id";
var src = "source";
var tgt = "target";

var numNotes = "status";

// Sets up and starts a replica set of three nodes to
// be used to test other functions on
// Returns a reference to the replica set
function createReplSet() {
    load("jstests/replsets/rslib.js");
    var replTest = new ReplSetTest( {name : 'testSet', nodes : 3});
    var nodes = replTest.startSet();
    replTest.initiate();
    return replTest;
}

// Takes in a connection of the form "host:port"
// and returns a complete graph of the connections
// associated with this node
function pingReplSet( host ){
   
    var conn = new Mongo( host );
    var db = conn.getDB("admin");
    var connStatus = true;
    var masterStats = db.runCommand( { "isMaster" : 1 } );
    
    var output = {};
    output["errors"] = new Array();
    output["warnings"] = new Array();
   
    if(masterStats["hosts"])	
    {
	//check if set name exists	
	if(!masterStats["setName"] || masterStats["setName"] == "")
	   output["warnings"] = "No replica set name noted for set";
	else
	    var setName = masterStats["setName"];

	output["members"] = masterStats["hosts"];

	//check connections between each pair of nodes in the replset	
	var serverSet = masterStats["hosts"];
	for(var i=0; i<serverSet.length; i++){
	    var curr_conn = new Mongo( serverSet[i] );
	    var curr_db = curr_conn.getDB("admin");
	    var curr_masterStats = curr_db.runCommand( { "isMaster" : 1 } ); 
	    
	    //Check if this node's replica set name matches primary's replica set name 
	    if( masterStats["setName"] && curr_masterStats["setName"] != setName )
		output["errors"].push("Replica set secondary " + serverSet[i] + 
		    " disagrees with primary " + primary + " on replica set name"); 
	   
	    //Check if this node is connected to the other nodes in the set 
	    var curr_stats = curr_db.runCommand( { "ping" : 1, hosts : serverSet } ) ;
	    for(j=0; j<serverSet.length; j++){
		if(curr_stats[serverSet[j]]["isConnected"] == false){
		    output["errors"].push("Replica set is not fully connected -- " + serverSet[i]
			+ " " + curr_stats[serverSet[j]]["connInfo"]); 
		    connStatus = false;	
		}  
	    }
	}	
	output["isFullyConnected"] = connStatus;
    }
    else
    {
	output["errors"].push("Not a member of replica set");
    }
    
    if(output["errors"].length == 0)
	output["errors"] = "none";
    if(output["warnings"].length == 0)
	output["warnings"] = "none";
 
    printjson(output);
}

function createShardedCluster() {

    // ShardingTest = function( testName, numShards, verboseLevel, numMongos, otherParams )
    // testName is the cluster name	
//    s = new ShardingTest( "shard1" , 3 , 0 , 3 );
//    s = new ShardingTest( {name:"shard1" , verbose:1 , mongos:2 , rs:{nodes : 3} , shards:4 , config:3 } );
    s = new ShardingTest( {name:"shard1" , rs:{nodes:3}} );    
   return s;			

}

// Takes in a connection of the form "host:port" and returns a complete graph of the connections
// associated with this node
function pingCluster( host , verbosity ) {
    try{ var conn = new Mongo( host ); }
    catch(e){
	print("Unable to connect to input host.");
	return;
    } 
    try{ var configDB = conn.getDB(config); }
    catch(e){
	print("Unable to connect to config database from input host.");
	return;
    }
    try{ var adminDB = conn.getDB("admin"); }
    catch(e){
	print("Unable to connect to admin database on input host.");
	return;
    }

    print("running...");

    var nodes = {};   
    var edges = {};
    var idMap = {};
    var index = 0; 
    var errors = {};
    var warnings = {};

    index = getShardServers( configDB , nodes , index , errors , warnings);
    index = getMongosServers( configDB , nodes , index , errors , warnings);
    index = getConfigServers( adminDB , nodes , index , errors , warnings);

    buildGraph( nodes , edges , errors , warnings );   
    buildIdMap( nodes , idMap );
    var diagnosis = diagnose( nodes , edges , errors , warnings );

//    printjson(nodes); 
    var currDate = new Date();
    var currTime = currDate.toUTCString(); 
    saveSnapshot( currTime , nodes , edges , idMap , errors , warnings );

//    var userView = buildUserView( diagnosis , verbosity ); 
//    printjson( userView );

    printjson( diagnosis );
    printjson( {"ok" : 1} );
}

function printEdges( edges ){


}

//currently unused
function buildUserView( diagnosis , verbosity ){
    var userView = {};
    userView["mongos"] = {};
    userView["shards"] = {};
    userView["config"] = {};
    return userView;
}
 
var ERR = {
    "CONFIG_DOWN" : "A config server is down",
    "SHARD_IS_STANDALONE_MONGOD" : "This shard is a standalone mongod instance, not replica set primary",
    "MISSING_REQ_CONN" : "Missing required connection to ",
    "MISSING_REC_CONN" : "Missing recommended connection to ",
    "CLIENT_CONNECTION_ERROR" : "Unable to make client connection",
    "NO_REPL_SET_NAME_NOTED" : "No replica set name noted",
    "CONFIG_SERVER_IS_ALSO_SHARD_SERVER" : "This mongod instance is a config server as well as shard server",
    "REPORTS_DIF_REPLSET_NAME" : "Reports different replset name from primary",
    "REPL_SET_NO_MASTER" : "No master found for replica set",
    "REPL_SET_NOT_CONN" : "Replica set not fully connected",
    "CAN'T_CONNECT_TO_REPL_SET_PRIMARY" : "Unable to connect to this primary's replica set",
    "NOT_CONNECTED_TO_REPL_SET" : "Not connected to its replica set"
}

function addError( key , msg , errors ){
    if( errors[ key ] == null ){
	errors[ key ] = new Array();
	errors[ key ].push( msg );
	return;
    }
    for (var i=0; i< errors[key].length; i++)
	if( errors[key][i] == msg )
	    return;
    errors[ key ].push( msg );
} 

function addWarning( key , msg , warnings ){
     if( warnings[ key ] == null ){
	warnings[ key ] = new Array();
	warnings[ key ].push( msg );
	return;
    }
    for (var i=0; i< warnings[key].length; i++)
	if( warnings[key][i] == msg )
	    return;
    warnings[ key ].push( msg );
}

var reqConnChart = {
    "mongos" : { "mongos":false , "config":true , "primary":true, "secondary":false },
    "config" : { "mongos":true , "config":true , "primary":true, "secondary":false },
    "primary" : { "mongos":true , "config":true , "primary":true, "secondary":false },
    "secondary" : { "mongos":false , "config":false , "primary":false, "secondary":false }
};
 
var recConnChart = {
    "mongos" : { "mongos":false , "config":false , "primary":false, "secondary":true },
    "config" : { "mongos":false , "config":false , "primary":false, "secondary":true },
    "primary" : { "mongos":false , "config":false , "primary":false, "secondary":true },
    "secondary" : { "mongos":true , "config":true , "primary":true, "secondary":true }
};

function isReqConn( src , tgt , nodes ){
    return reqConnChart[ nodes[src]["role"] ][ nodes[tgt]["role"] ];
}

function isRecConn( src , tgt , nodes ){
    return recConnChart[ nodes[src]["role"] ][ nodes[tgt]["role"] ];
}

function diagnose( nodes , edges , errors , warnings){
    var diagnosis = {};

    //check edges
    for( var src in edges ){
	for( var tgt in edges[src] ){
	    if( edges[ src ][ tgt ]["isConnected"] == false){
		if( isReqConn( src , tgt , nodes ) )
		    addError( nodes[src]["key"] , ERR["MISSING_REQ_CONN"] + nodes[tgt]["key"] , errors );  
		if( isRecConn( src , tgt , nodes ) )
		    addWarning( nodes[src]["key"] , ERR["MISSING_REC_CONN"] + nodes[tgt]["key"] , warnings ); 
	    }
	}
    }

    //check replica sets
    for(var curr in nodes){
	if( nodes[curr]["role"] == "primary" )
	    checkReplSet( curr , nodes , errors , warnings );
    }

    //check if config server is also a shard server
    var configSvrs = new Array(); 
    for(var curr in nodes){
	if( nodes[curr]["role"] == "config" )
	    configSvrs.push( curr );
    }
    for(var curr in nodes){
	for(var i=0; i< configSvrs.length; i++){
	    if( nodes[curr]["key"] == nodes[configSvrs[i]]["key"] && nodes[curr]["role"] != "config")
		addWarning( nodes[curr]["key"] , ERR["CONFIG_SERVER_IS_ALSO_SHARD_SERVER"] , warnings );
	}
    }	
    
    diagnosis["errors"] = errors;
    diagnosis["warnings"] = warnings;
    
    return diagnosis;

}

function checkReplSet( primary , nodes , errors , warnings ){

    var errorArray = new Array();

    try{
    var conn = new Mongo( nodes[primary]["hostName"] );
    var db = conn.getDB("admin");
    }
    catch(e){
	addError( nodes[primary]["key"] , ERR["CAN'T_CONNECT_TO_REPL_SET_PRIMARY"] , errors );
	return;
    }

    var connStatus = true;
    var masterStats = db.runCommand( { "isMaster" : 1 } );
      
    if(masterStats["hosts"])	
    {
	var setName;
	if(!masterStats["setName"] || masterStats["setName"] == ""){
	    addWarning( nodes[primary]["key"]  , ERR["NO_REPL_SET_NAME_NOTED"] , warnings);
	    setName = null;
	}
	else
	    setName = masterStats["setName"];
	var serverSet = masterStats["hosts"];
	for(var i=0; i<serverSet.length; i++){
	    try{ 
		var curr_conn = new Mongo( serverSet[i] );
		var curr_db = curr_conn.getDB("admin"); 
		var curr_masterStats = curr_db.runCommand( { "isMaster" : 1 } ); 
		//Check if replica set name matches primary's replica set name 
		if( setName != null && curr_masterStats["setName"] != setName )
		    addError( nodes[primary]["replSetName"] , ERR["NODES_DIAGREE_ON_REPLSET_NAME"] , errors ); 
		//Check if current node is connected to the other nodes in the set 
		var curr_stats = curr_db.runCommand( { "ping" : 1, hosts : serverSet } ) ;
		for(j=0; j<serverSet.length; j++)
		    if(curr_stats[ serverSet[j] ]["isConnected"] == false)
			addError( nodes[primary]["replSetName"] , ERR["REPL_SET_NOT_CONN"] , errors );
	    }
	    catch(e) { }  
	}	
    }
 
}

//Although the {ping} function takes an array of hosts to ping, this function
//pings each node one at a time (passes an array of one element to {ping})
//for a few reasons:
    // 1) forwarding in the ping function has not been implemented yet
    // 2) 
function buildGraph( nodes , edges , errors , warnings ){
    for( var srcNode in nodes){
	edges[ srcNode ] = {}; 
	for( var tgtNode in nodes ) {	
	    if(srcNode != tgtNode){ 
		var newEdge = {};
		try{	
		    var conn = new Mongo( nodes[srcNode]["hostName"] );
		    var admin = conn.getDB("admin"); 
		    var pingInfo = 
			admin.runCommand( { "ping" : 1, hosts : [nodes[tgtNode]["hostName"]] } );
		    if( pingInfo[ nodes[tgtNode]["hostName"] ][ "isConnected" ] == false){
			newEdge["isConnected"] = false;
			newEdge["errmsg"] = pingInfo[ nodes[tgtNode]["hostName"] ][ "errmsg" ];	
		    }
		    else{
			newEdge["isConnected"] = true;
			newEdge["pingTimeMicrosecs"] = 
			    pingInfo[ nodes[tgtNode]["hostName"] ]["pingTimeMicrosecs"]; 
			//more ping info can be added here later 
		    }
		    newEdge["bytesSent"] = pingInfo[ nodes[tgtNode]["hostName"] ]["bytesSent"];
		    newEdge["bytesReceived"] = pingInfo[ nodes[tgtNode]["hostName"] ]["bytesReceived"]; 
		    newEdge["incomingSocketExceptions"] =
			pingInfo[ nodes[tgtNode]["hostName"] ]["incomingSocketExceptions"];	
		    newEdge["outgoingSocketExceptions"] =
			pingInfo[ nodes[tgtNode]["hostName"] ]["outgoingSocketExceptions"]; 
		    newEdge["clientSocketExceptions"] = 
			pingInfo[ nodes[tgtNode]["hostName"] ]["clientSocketExceptions"]; 
		    newEdge["fortesting"] = pingInfo[ nodes[tgtNode]["hostName"] ]["fortesting"]; 
		    edges[ srcNode ][ tgtNode ] = newEdge;	    
		}
		catch(e){
	//	    edges[ srcNode ][ tgtNode ] = e;
		    addError( nodes[srcNode]["key"] , ERR["CLIENT_CONNECTION_ERROR"] , errors );	
	    	} 
	    }
	}
    }
}

function getConfigServers( adminDB , nodes , index , errors , warnings ){
    try{
	configSvr = adminDB.runCommand( { getCmdLineOpts : 1 });
	if(configSvr != null && configSvr != ""){
	    var id = index;
	    index++;
	    nodes[id] = {};
	    nodes[id]["hostName"] = configSvr["parsed"]["configdb"];
	    nodes[id]["machine"] = ""; //to be expanded later
	    nodes[id]["process"] = "mongod";	
	    nodes[id]["errors"] = new Array();
	    nodes[id]["warnings"] = new Array();
	    nodes[id]["role"] = "config";
	    nodes[id]["key"] = nodes[id]["hostName"] 
		+ "_" + nodes[id]["machine"] 
		+ "_" + nodes[id]["process"];
	    collectClientInfo( id , nodes , errors , warnings );
	}
	else
	    addWarning( "cluster" , ERR["NO_CONFIG_SERVER_NOTED"] , warnings );
    } catch(e) {} 
        return index;
}

function getMongosServers( configDB , nodes , index , errors , warnings ){
    try{
	configDB.mongos.find().forEach( function(doc) {
	    var id = index;
	    index++;
	    nodes[id] = {};
	    nodes[id]["hostName"] = doc["_id"];
	    nodes[id]["machine"] = ""; //to be expanded later
	    nodes[id]["process"] = "mongos";
	    nodes[id]["role"] = "mongos";	
	    nodes[id]["errors"] = new Array();
	    nodes[id]["warnings"] = new Array();
	    nodes[id]["key"] = nodes[id]["hostName"] 
				+ "_" + nodes[id]["machine"] 
				+ "_" + nodes[id]["process"];
	    collectClientInfo( id , nodes , errors , warnings );
	});
    } catch(e) { addError( "cluster" , ERR["CONFIG_DOWN"] , errors ); }
    return index;
}

function getShardServers( configDB , nodes , index , errors , warnings ){
	try{
        configDB.shards.find().forEach( function(doc) {
	// if the shard is a replica set 
	// do string parsing for shard servers
	// originally in format "shard-01/lcalhost:30000,localhost:30001,localhost:30002"	
	if( (startPos = doc["host"].indexOf("/") ) != -1) {
	    var shardName = doc["host"].substring(0 , startPos); 
	    var hosts = doc["host"].substring( startPos + 1 ).split(",");
	    for(var i=0; i<hosts.length; i++) {
		var id = index;
		index++;
		nodes[id] = {};
		nodes[id]["hostName"] = hosts[i];
		nodes[id]["machine"] = ""; //to be expanded later
		nodes[id]["process"] = "mongod";
//		nodes[id]["errors"] = new Array();
//		nodes[id]["warnings"] = new Array();
		nodes[id]["shardName"] = shardName;
		nodes[id]["key"] = nodes[id]["hostName"] 
				    + "_" + nodes[id]["machine"]
				    + "_" + nodes[id]["process"];
		if(i == 0)
		    nodes[id]["role"] = "primary";	
		else
		    nodes[id]["role"] = "secondary";
		collectClientInfo( id , nodes , errors , warnings );
	    }	
	}
	//if the shard has a standalone mongod instance 
	else{
	    var id = index;
	    index++;
	    nodes[id] = {};
	    nodes[id]["hostName"] = doc["host"];
	    nodes[id]["machine"] = ""; //to be expanded later
	    nodes[id]["process"] = "mongod";
	    nodes[id]["role"] = "primary";	
	    nodes[id]["errors"] = new Array();
	    nodes[id]["warnings"] = new Array();
	    nodes[id]["key"] = nodes[id]["hostName"] 
				+ "_" + nodes[id]["machine"] 
				+ "_" + nodes[id]["process"];
	    collectClientInfo( id , nodes , errors , warnings ); 
	    addWarning( nodes[id]["key"] , ERR["SHARD_IS_STANDALONE_MONGOD"] , warnings );
	}
    });
    } catch (e) {
	addError( "cluster" , ERR["CONFIG_DOWN"] , errors );
    }
    return index;	
}

function collectClientInfo( id , nodes , errors , warnings ){
    //refine error catching
    try{ var currConn = new Mongo( nodes[id]["hostName"] ); 
	try{ var currAdminDB = currConn.getDB("admin");
	    
	    // all nodes collect the following:
	    try{ nodes[id]["hostInfo"] = currAdminDB.runCommand({ "hostInfo" : 1 }); }
	    catch(e) { nodes[id]["hostInfo"] = null; }
	    try{ nodes[id]["serverStatus"] = currAdminDB.runCommand({ "serverStatus" : 1 }); }
	    catch(e) { nodes[id]["serverStatus"] = null; } 
	    /* add check for no config server noted */	  

	    // only mongod nodes collect the following:  
	    if( nodes[id]["process"] == "mongod" ){ 
		try{ nodes[id]["buildInfo"] = currAdminDB.runCommand({ "buildInfo" : 1 }); }
		catch(e) { nodes[id]["buildInfo"] = null; }    	
	    }
	   
	    // only shard mongod nodes collect the following:
	    if( nodes[id]["role"] == "primary" || nodes[id]["role"] == "secondary" ){
		try{
		    var masterStats = currAdminDB.runCommand( { "isMaster" : 1 } );
		    try{ nodes[id]["replSetName"] = masterStats["setName"]; }
		    catch(e){
			addWarning( nodes[id]["key"] , ERR["NO_REPL_SET_NAME_NOTED"] , warnings );
			nodes[id]["replSetName"] = null;
		    }	
		}
		catch(e){} 
	    }
	} catch(e) { /*couldn't connect to admin database*/ }

	try{ var currTestDB = currConn.getDB("test");
	    // all nodes collect the following:	
	    try{ nodes[id]["shardVersion"] = currAdminDB.test.runCommand({ "getShardVersion" : 1 }); }
	    catch(e) { nodes[id]["getShardVersion"] = null; }
	} catch(e) { /*couldn't connect to test database*/ } 
    
    } catch(e) { 
	addWarning( nodes[id]["key"] , "Couldn't collect client info" , warnings );
	/*couldn't connect to host*/
     } 

    return;
}

function buildIdMap( nodes , idMap ){
    for ( var curr in nodes ){
	idMap[ nodes["key"] ] = curr;
    }
}

function showAllNodes( verbosity ){ 
    printjson( history["allNodes"] ); 
}

function saveSnapshot( time , nodes , edges , idMap , errors , warnings){
    history["snapshots"][ time ] = { 
	"nodes" : nodes , 
	"edges" : edges ,
	"idMap" : idMap ,
	"errors" : errors ,
	"warnings" : warnings
	}; 
    //add any new nodes to the list of all nodes that have ever existed 
    for ( var curr in idMap )
	if( history["allNodes"][ curr ] == null){
	    history["allNodes"][ curr ] = {};
	    history["allNodes"][ curr ]["status"] = "alive"; 
	    history["allNodes"][ curr ]["previousRoles"] = new Array();
	    history["allNodes"][ curr ]["currentRole"] = nodes [ idMap[curr] ]["role"];
	}
    //mark any nodes that previously existed but no longer exist as dead
    for ( var curr in history["allNodes"] )
	if( idMap[ curr ] == null )
	    history["allNodes"][ curr ] = "dead";
}

function showHistory(){
    var times = new Array();
    for (var moment in history["snapshots"])
	times.push( moment );
    printjson( times );
}

function calculateStats(){

    var count=0;
    for (var moment in history["snapshots"])
	count++;
    if(count < 1){
	print("Not enough snapshots to calculate statistics. Please ping cluster at least once.");
	return;
    }

    var edgeStats = {};
    for( var srcName in history["allNodes"] ){
	edgeStats[ srcName ] = {};
	for( var tgtName in history["allNodes"]){
	    if( srcName != tgtName){ 
		edgeStats[ srcName ][ tgtName ] = {
		    "numPingAttempts" : 0,
		    "numSuccessful" : 0,
		    "numFailed" : 0,	
		    "percentageIsConnected" : 0,
		    "avgIncomingSocketExceptions" : 0,
		    "avgOutgoingSocketExceptions" : 0,
//		    "totalIncomingSocketExceptions" : 0,
//		    "totalOutgoingSocketExceptions" : 0,
		    "avgBytesSent" : 0,
		    "avgBytesReceived" : 0,
		    "totalBytesSent" : 0,
		    "totalBytesReceived" : 0,
		    "maxPingTimeMicrosecs" : null,
		    "minPingTimeMicrosecs" : null,
		    "sumPingTimeMicrosecs" : 0,
		    "avgPingTimeMicrosecs" : null,
		    "pingTimeStdDeviation" : null,
		    "subtractMeanSquaredSum" : 0
		};
	    }
    	}	
    }

    // max ping time, min ping time, num ping attempts, num successful, num failed, num socketexceptions
    for(var moment in history["snapshots"]){	
	var currEdges = history["snapshots"][moment]["edges"];	

	for( var srcName in history["allNodes"] ){
	    for( var tgtName in history["allNodes"] ){
	
		if( srcName != tgtName){	
		    var src = history["snapshots"][moment]["idMap"][ srcName ];	
		    var tgt = history["snapshots"][moment]["idMap"][ tgtName ];
		   
		    // if edge existed in this snapshot 
		    if( currEdges[ src ][ tgt ] != null ){
			edgeStats[ srcName ][ tgtName ]["numPingAttempts"]++;

// right now, each ping returns the total number of
// incoming/outgoing socket exceptions already
// when/if later that changes to showing only the 
// number since the last ping, then this can be re-added
/*			edgeStats[ srcName ][ tgtName ]["totalIncomingSocketExceptions"] 
			    = parseInt(edgeStats[ srcName ][ tgtName ]["totalIncomingSocketExceptions"])
			    + parseInt("" + currEdges[ src ][ tgt ]["incomingSocketExceptions"]);

			edgeStats[ srcName ][ tgtName ]["totalOutgoingSocketExceptions"] 
			    = parseInt(edgeStats[ srcName ][ tgtName ]["totalOutgoingSocketExceptions"])
			    + parseInt("" + currEdges[ src ][ tgt ]["outgoingSocketExceptions"]);
*/

			edgeStats[ srcName ][ tgtName ]["totalBytesSent"] 
			    = parseInt(edgeStats[ srcName ][ tgtName ]["totalBytesSent"])
			    + parseInt("" + currEdges[ src ][ tgt ]["bytesSent"]);

			edgeStats[ srcName ][ tgtName ]["totalBytesReceived"] 
			    = parseInt(edgeStats[ srcName ][ tgtName ]["totalBytesReceived"])
			    + parseInt("" + currEdges[ src ][ tgt ]["bytesReceived"]);

			if( currEdges[ src ][ tgt ]["isConnected"] == true){

			    edgeStats[ srcName ][ tgtName ]["numSuccessful"]++;

			    var pingTime = currEdges[ src ][ tgt ]["pingTimeMicrosecs"];
			
			    if( edgeStats[ srcName ][ tgtName ]["maxPingTimeMicrosecs"] == null 
				|| pingTime > edgeStats[ srcName ][ tgtName ]["maxPingTimeMicrosecs"])
				edgeStats[ srcName ][ tgtName ]["maxPingTimeMicrosecs"] = parseInt(pingTime);	
			    if( edgeStats[ srcName ][ tgtName ]["minPingTimeMicrosecs"] == null 
				|| pingTime < edgeStats[ srcName ][ tgtName ]["minPingTimeMicrosecs"])
				edgeStats[ srcName ][ tgtName ]["minPingTimeMicrosecs"] = parseInt(pingTime);	
			    edgeStats[ srcName ][ tgtName ]["sumPingTimeMicrosecs"]  
				= parseInt(edgeStats[ srcName ][ tgtName ]["sumPingTimeMicrosecs"]) 
				+ parseInt(pingTime);	
			}
			else
			    edgeStats[ srcName ][ tgtName ]["numFailed"]++; 
		    }
		}
	    }
	}	
    }
    // avg ping time and percentage connected 
    for( var srcName in history["allNodes"] ){
	for( var tgtName in history["allNodes"] ){

	    if( srcName != tgtName){

		if( edgeStats[ srcName ][ tgtName ]["numSuccessful"] > 0 ){
		    edgeStats[ srcName ][ tgtName ]["avgPingTimeMicrosecs"] 
			= parseFloat( edgeStats[ srcName ][ tgtName ]["sumPingTimeMicrosecs"]) 
			/ parseFloat( edgeStats[ srcName ][ tgtName ]["numSuccessful"]);	
		}	

		edgeStats[ srcName ][ tgtName ]["avgIncomingSocketExceptions"] = 
		    parseFloat( edgeStats[ srcName ][ tgtName ]["totalIncomingSocketExceptions"])
		    / parseFloat( edgeStats[ srcName ][ tgtName ]["numPingAttempts"] );

		edgeStats[ srcName ][ tgtName ]["avgOutgoingSocketExceptions"] = 
		    parseFloat( edgeStats[ srcName ][ tgtName ]["totalOutgoingSocketExceptions"])
		    / parseFloat( edgeStats[ srcName ][ tgtName ]["numPingAttempts"] );

		edgeStats[ srcName ][ tgtName ]["avgBytesSent"] = 
		    parseFloat( edgeStats[ srcName ][ tgtName ]["totalBytesSent"])
		    / parseFloat( edgeStats[ srcName ][ tgtName ]["numPingAttempts"] );

		edgeStats[ srcName ][ tgtName ]["avgBytesReceived"] = 
		    parseFloat( edgeStats[ srcName ][ tgtName ]["totalBytesReceived"])
		    / parseFloat( edgeStats[ srcName ][ tgtName ]["numPingAttempts"] );


		edgeStats[ srcName ][ tgtName ]["percentageIsConnected"] = 100  
		    * parseFloat( edgeStats[ srcName ][ tgtName ]["numSuccessful"]) 
		    / parseFloat( edgeStats[ srcName ][ tgtName ]["numPingAttempts"]);	
	    }
	}
    }

    // ping time standard deviation
    for(var moment in history["snapshots"]){	
	var currEdges = history["snapshots"][moment]["edges"];	
	for( var srcName in history["allNodes"] ){
	    for( var tgtName in history["allNodes"] ){
		if( srcName != tgtName && edgeStats[ srcName ][ tgtName ]["numSuccessful"] > 0){	
		    var src = history["snapshots"][moment]["idMap"][ srcName ];	
		    var tgt = history["snapshots"][moment]["idMap"][ tgtName ];
		    if( currEdges[ src ][ tgt ] != null && currEdges[ src ][ tgt ]["isConnected"] != false){ 
			edgeStats[ srcName ][ tgtName ]["subtractMeanSquaredSum"] 
			    = parseFloat( edgeStats[ srcName ][ tgtName ]["subtractMeanSquaredSum"])
			    + (parseFloat( currEdges[ src ][ tgt ]["pingTimeMicrosecs"] ) 
				- parseFloat( edgeStats[ srcName ][ tgtName ]["avgPingTimeMicrosecs"])) 
			    * (parseFloat( currEdges[ src ][ tgt ]["pingTimeMicrosecs"] ) 
				- parseFloat( edgeStats[ srcName ][ tgtName ]["avgPingTimeMicrosecs"])); 
		    }	  
		} 
	    }
	}
    }
    for( var srcName in history["allNodes"] ){
	for( var tgtName in history["allNodes"] ){
	    if( srcName != tgtName ){
		if( edgeStats[ srcName ][ tgtName ]["numSuccessful"] > 0)
		    edgeStats[ srcName ][ tgtName ]["pingTimeStdDeviation"] 
			= Math.sqrt( parseFloat(edgeStats[ srcName ][ tgtName ]["subtractMeanSquaredSum"]) 
			/ parseFloat(edgeStats[ srcName ][ tgtName ]["numSuccessful"])); 
		delete edgeStats[ srcName ][ tgtName ]["subtractMeanSquaredSum"];	
		delete edgeStats[ srcName ][ tgtName ]["sumPingTimeMicrosecs"]; 
	    }	
	}
    }
    
    history["stats"] = edgeStats; 
    printjson("{ ok : 1 }");
 
    return;   
}

function showStats(){
//    for( var node in history["stats"] )
    printjson( history["stats"] )
}

//deltas are defined as the change from the previous time point to the current time point
function calculateDeltas(){
    var deltas = {};   
    var count=0;
    var prevMoment; 
    for( var moment in history["snapshots"] ){	
	
	if(count != 0){
	    var currSnapshot = history["snapshots"][moment];	    
	    var prevSnapshot = history["snapshots"][prevMoment];
	    
	    //set up this dif
	    deltas[ moment ] = {};
	    deltas[ moment ]["prevTime"] = prevMoment;
	    deltas[ moment ]["newErrors"] = new Array() 
	    deltas[ moment ]["newWarnings"] = new Array();
	    deltas[ moment ]["newNodes"] = new Array();
	    deltas[ moment ]["removedErrors"] = new Array();
	    deltas[ moment ]["removedWarnings"] = new Array();
	    deltas[ moment ]["removedNodes"] = new Array(); 

	    //check for added/removed nodes
    	    for( var currNode in currSnapshot["idMap"] )
		if( prevSnapshot["idMap"][ currNode ] == null )
		    deltas[ moment ]["newNodes"].push( currNode );
	    for( var prevNode in prevSnapshot["idMap"] )
		if( currSnapshot["idMap"][ prevNode ] == null )
		    deltas[ moment ]["removedNodes"].push( prevNode );	    	
	
	    //check for added/removed errors and warnings
	    for( var src in currSnapshot["errors"] ){
		for(var i=0; i<currSnapshot["errors"][src].length; i++){
		    if( snapshotHasError( src , currSnapshot["errors"][src][i] , prevSnapshot ) )
			deltas[ moment ]["newErrors"].push( src + ":" + currSnapshot["errors"][ src ][i]);
		}
	    }
/*
	    for( var prevError in prevSnapshot["errors"] )
		if( currSnapshot["errors"][ prevError ] == null
		    || currSnapshot["errors"][ prevError ] != prevSnapshot["errors"][ prevError ] )
		    deltas[ moment ]["removedErrors"].push( prevError + ":" + prevSnapshot["errors"][ prevError ]); 
    	    for( var currWarning in currSnapshot["warnings"] )
		if( prevSnapshot["warnings"][ currWarning ] == null
		    || prevSnapshot["warnings"][ currWarning ] != currSnapshot["warnings"][ currWarning ] )
		    deltas[ moment ]["newWarnings"].push( currWarning + ":" + currSnapshot["warnings"][ currWarning ]);
	    for( var prevWarning in prevSnapshot["warnings"] )
		if( currSnapshot["warnings"][ prevWarning ] == null
		    || currSnapshot["warnings"][ prevWarning ] != prevSnapshot["warnings"][ prevWarning ] )
		    deltas[ moment ]["removedWarnings"].push( prevWarning + ":" + prevSnapshot["warnings"][ prevWarning ]); 
*/
	}
	prevMoment = moment; 
	count++;  
    }		
    if(count < 2)
	print("Not enough snapshots to calculate deltas. Please ping cluster at least twice.");
    else{
	history["deltas"] = deltas; 
	printjson("{ ok : 1 }");
    }
}

function snapshotHasError( src , error , prevSnapshot ){
    if( prevSnapshot["errors"][ src ] == null)
	return false;
    for(var i=0; i<prevSnapshot["errors"][ src ].length; i++)
	if( error == prevSnapshot["errors"][src][i] )
	    return true;     
    return false;
}

function snapshotHasWarning(){

}

function showDeltas(){
    printjson( history["deltas"] );
}

function calculateDeltasBetween( time1 , time2 ){

    var deltas = {};   
    var count=0;
    var prevMoment; 

    if( new Date(time1) == null  || new Date(time2) == null ){
	print("Please enter valid Date strings");
	return;
    }
	
    if( time2 <= time1 ){
	print("Please enter two distinct times in order (earlier , later)");
	return;
    }

    for( var moment in history["snapshots"] ){	
	if(count != 0 && moment >= time1 && moment <= time2){
	    var currSnapshot = history["snapshots"][moment];	    
	    var prevSnapshot = history["snapshots"][prevMoment];
	    
	    //set up this dif
	    deltas[ moment ] = {};
	    deltas[ moment ]["prevTime"] = prevMoment;
	    deltas[ moment ]["newErrors"] = new Array() 
	    deltas[ moment ]["newWarnings"] = new Array();
	    deltas[ moment ]["newNodes"] = new Array();
	    deltas[ moment ]["removedErrors"] = new Array();
	    deltas[ moment ]["removedWarnings"] = new Array();
	    deltas[ moment ]["removedNodes"] = new Array(); 

	    //check for added/removed nodes
	    for( var currNode in currSnapshot["idMap"] )
		if( prevSnapshot["idMap"][ currNode ] == null )
		    deltas[ moment ]["newNodes"].push( currNode );
	    for( var prevNode in prevSnapshot["idMap"] )
		if( currSnapshot["idMap"][ prevNode ] == null )
		    deltas[ moment ]["removedNodes"].push( prevNode );	    	
	
	    //check for added/removed errors and warnings
	    for( var currError in currSnapshot["errors"] )
		if( prevSnapshot["errors"][ currError ] == null
		    || prevSnapshot["errors"][ currError ] != currSnapshot["errors"][ currError ] )
		    deltas[ moment ]["newErrors"].push( currError + ":" + currSnapshot["errors"][ currError ]);
	    for( var prevError in prevSnapshot["errors"] )
		if( currSnapshot["errors"][ prevError ] == null
		    || currSnapshot["errors"][ prevError ] != prevSnapshot["errors"][ prevError ] )
		    deltas[ moment ]["removedErrors"].push( prevError + ":" + prevSnapshot["errors"][ prevError ]); 
	    for( var currWarning in currSnapshot["warnings"] )
		if( prevSnapshot["warnings"][ currWarning ] == null
		    || prevSnapshot["warnings"][ currWarning ] != currSnapshot["warnings"][ currWarning ] )
		    deltas[ moment ]["newWarnings"].push( currWarning + ":" + currSnapshot["warnings"][ currWarning ]);
	    for( var prevWarning in prevSnapshot["warnings"] )
		if( currSnapshot["warnings"][ prevWarning ] == null
		    || currSnapshot["warnings"][ prevWarning ] != prevSnapshot["warnings"][ prevWarning ] )
		    deltas[ moment ]["removedWarnings"].push( prevWarning + ":" + prevSnapshot["warnings"][ prevWarning ]); 
	}
	prevMoment = moment; 
	count++;  
    }		
    if(count < 2)
	print("Not enough snapshots to calculate deltas. Please provide at least two distinct times.");
    else
	printjson(deltas); 

}

function clearHistory(){
    history["snapshots"] = {};
    history["allNodes"] = {};
}


