var history = {};
history["snapshots"] = {};
history["allNodes"] = {};

/*stand-in names for strings used in output*/
var allConn = "allConnections"; 
var outConn = "outConnections";
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
    var masterStats = db.runCommand( { ismaster : 1 } );
    
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
	    var curr_masterStats = curr_db.runCommand( { ismaster : 1 } ); 
	    
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
    //s = new ShardingTest( "shard1" , 3 , 0 , 3 );
//    s = new ShardingTest( {name:"shard1" , verbose:1 , mongos:3 , rs:{nodes : 3} , shards:6 , config:3 } );
    s = new ShardingTest( {name:"shard1" , rs:{nodes:3}} );    
   return s;			

}

// Takes in a connection of the form "host:port" and returns a complete graph of the connections
// associated with this node
function pingCluster( host , verbosity ) {

    try{
	var conn = new Mongo( host );
    }
    catch(e){
	printjson(e);
	return;
    } 
    try{
     	var configDB = conn.getDB(config); //what exactly is this "config"?
    }
    catch(e){
	printjson(e);
	return;
    }
    try{
	var adminDB = conn.getDB("admin");
    }
    catch(e){
	printjson(e);
	return;
    }

    var nodes = {};   
    var edges = {};
    var idMap = {};
    var index = 0; 
    var errors = {};
    var warnings = {};

    index = getShardServers( configDB , nodes , index , idMap);
    index = getMongosServers( configDB , nodes , index , idMap);
    index = getConfigServers( adminDB , nodes , index , idMap);
  
    buildGraph( nodes , edges , errors , warnings );   
    buildIdMap( nodes , idMap );
    var diagnosis = diagnose( nodes , edges , errors , warnings );

    printjson(edges);  
 
    var currDate = new Date();
    var currTime = currDate.toUTCString(); 
    saveSnapshot( currTime , nodes , edges , idMap , errors , warnings );

//    var userView = buildUserView( diagnosis , verbosity ); 
//    printjson( userView );

    printjson( diagnosis );
    printjson( {"ok" : 1} );
}

function buildUserView( diagnosis , verbosity ){

    var userView = {};
    userView["mongos"] = {};
    userView["shards"] = {};
    userView["config"] = {};

    userView["mongos"]["status"] = getStatus( diagnosis["mongos"] , "hostName" );
    userView["config"]["status"] = getStatus( diagnosis["config"] , "hostName" );
    userView["shards"]["status"] = getStatus( diagnosis["shards"] , "shardName" );

    if(verbosity == "v" || verbosity == "vv"){
	
	userView["mongos"]["list"] = {};
    	diagnosis["mongos"].map( function(node){
	    userView["mongos"]["list"][ node["hostName"] ] = getMemberStatus( node );
	});
   
	userView["config"]["list"] = {};
	diagnosis["config"].map( function(node){
	    userView["config"]["list"][ node["hostName"] ] = getMemberStatus( node );
	});

	userView["shards"]["list"] = {};
	diagnosis["shards"].map( function(node){
	    
	    if( verbosity == "vv"){
		userView["shards"]["list"][ node["shardName"] ] = {};
		node["primary"].map( function(member){
		    userView["shards"]["list"][ node["shardName"] ][ member["hostName"] ]
			= getMemberStatus( member );	
		});
		node["secondary"].map( function(member){
		    userView["shards"]["list"][ node["shardName"] ][ member["hostName"] ]
			= getMemberStatus( member );	
		}); 
	    } 
	    
	    else
		userView["shards"]["list"][ node["shardName"] ] = getMemberStatus( node );
	});
    }

    return userView;
}

function getMemberStatus( node ){
    var myStatus = {};
    if(node["status"]["warnings"].length > 0 && node["status"]["errors"].length > 0){
	myStatus["errors"] = node["status"]["errors"];
	myStatus["warnings"] = node["status"]["warnings"];	
    }
    else if( node["status"]["warnings"].length > 0)
    	myStatus["warnings"] = node["status"]["warnings"];
    else if( node["status"]["errors"].length > 0)
	myStatus["errors"] = node["status"]["errors"];
    else
	myStatus = "ok";
    return myStatus;
}

var PROCESS_ERR = {
    "HAS_WARNING" : "Warnings at : ",
    "HAS_ERROR" : "Errors at : ",
    "SHARD_HAS_WARNING" : "Warnings with shard at : ",
    "SHARD_HAS_ERROR" : "Warnings with shard at : "
};

function getStatus( array , nameType ){
    var myStatus = {};
    var warnings = new Array();
    var errors = new Array();
 
    array.map( function(node){
	if( node["status"]["ok"] != 1 ){
	    if( node["status"]["warnings"].length > 0 )
		warnings.push( PROCESS_ERR["HAS_WARNING"] + node[nameType] );
	    if( node["status"]["errors"].length > 0 )
		errors.push( PROCESS_ERR["HAS_ERROR"] + node[nameType] );	
	}	
    });

    if(warnings.length > 0 && errors.length > 0){
	myStatus["warnings"] = warnings;
	myStatus["errors"] = errors;
    }
    else if(warnings.length > 0)
	myStatus["warnings"] = warnings;
    else if(errors.length > 0)
	myStatus["errors"] = errors; 
    else
	myStatus["ok"] = 1;

    return myStatus;
}
 
function diagnoseShard( node ){ 
  
    var errors = new Array();
    var warnings = new Array();

    node["primary"].map( function(member){
	if( member["status"]["ok"] != 1){
	    if( member["status"]["warnings"].length > 0)
		warnings.push( PROCESS_ERR["HAS_WARNING"] + member["hostName"] );
	    if( member["status"]["errors"].length > 0)
		errors.push( PROCESS_ERR["HAS_ERROR"] + member["hostName"] );
	}
    }); 

    if(node["primary"].length < 1)
	errors.push( PROCESS_ERR["NO_PRIMARY"] );
/*    else    
	checkReplSet( node["primary"][0] , warnings , errors )
*/
    var myStatus = {};
    if( warnings.length == 0 && errors.length == 0)
	myStatus["ok"] = 1; 
    else
	myStatus["ok"] = 0;
    myStatus["warnings"] = warnings;
    myStatus["errors"] = errors;
 
    return myStatus;	
}

var ERR = {
    "MISSING_REQ_CONNECTION" : "Missing required connection",
    "MISSING_REC_CONNECTION" : "Missing recommended connection",
    "CLIENT_CONN_ERROR" : "Client unable to make connection to ",
    "NO_REPL_SET_NAME_NOTED" : "No replica set name noted"
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
		    errors[ nodes[src]["key"] + " -> " + nodes[tgt]["key"] ]  =  ERR["MISSING_REQ_CONNECTION"]; 
		if( isRecConn( src , tgt , nodes ) )
		    warnings[ nodes[src]["key"] + " -> " + nodes[tgt]["key"] ] = ERR["MISSING_REC_CONNECTION"]; 
	    }
	}
    }
    //check replica sets

    //check if config server is also a shard server
   
    var configSvrs = new Array(); 
    for(var curr in nodes){
	if( nodes[curr]["role"] == "config" )
	    configSvrs.push( nodes[curr]["hostName"] );
    }


/*
    for(var curr in nodes){
	if( nodes[curr]["role"] 
*/
    diagnosis["errors"] = errors;
    diagnosis["warnings"] = warnings;
    
    return diagnosis;

}

function pingShardedReplSet( host ) {

    var conn = new Mongo( host );
    var db = conn.getDB("admin");
    var connStatus = true;
    var masterStats = db.runCommand( { ismaster : 1 } );
       
    if(masterStats["hosts"])	
    {
	if(!masterStats["setName"] || masterStats["setName"] == "")
	    warnings["primmary"].push("No replica set name noted for set with primary " + primary);
	else
	   var setName = masterStats["setName"];
	var serverSet = masterStats["hosts"];
	for(var i=0; i<serverSet.length; i++){
	    var curr_conn = new Mongo( serverSet[i] );
	    var curr_db = curr_conn.getDB("admin");
	    var curr_masterStats = curr_db.runCommand( { ismaster : 1 } ); 
	    //Check if replica set name matches primary's replica set name 
	    if( masterStats["setName"] && curr_masterStats["setName"] != setName )
		errors["primary"].push("Replica set secondary " + serverSet[i] + 
		    " disagrees with primary " + primary + " on replica set name"); 
	    //Check if current node is connected to the other nodes in the set 
	    var curr_stats = curr_db.runCommand( { "ping" : 1, hosts : serverSet } ) ;
	    for(j=0; j<serverSet.length; j++){
		if(curr_stats[serverSet[j]]["isConnected"] == false){
		    errors["primary"].push("A replica set is not fully connected -- " + serverSet[i]
			+ " " + curr_stats[serverSet[j]]["connInfo"]); 
		connStatus = false;	
		}  
	    }
	}	
     }
    return connStatus;
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
		    newEdge["totalSocketExceptions"] =
			    pingInfo[ nodes[tgtNode]["hostName"] ]["numPastSocketExceptions"];	
		    newEdge["connectionSocketExceptions"] = 
			pingInfo[ nodes[tgtNode]["hostName"] ]["numSocketExceptions"]; 
		    edges[ srcNode ][ tgtNode ] = newEdge;	    
		}
		catch(e){
	//	    edges[ srcNode[id] ][ tgtNode[id] ] = 
	//		ERR["CLIENT_CONN_ERR"] + tgtNode["hostName"];
	//  	   edges[ srcNode ][ tgtNode ] = e;
		    errors[ nodes[srcNode]["key"] + "->" + nodes[tgtNode]["key"] ] = e;	
	    	} 
	    }
	}
    }
}

function getConfigServers( adminDB , nodes , index ){

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
    	nodes[id]["key"] = nodes[id]["hostName"] + "_" + nodes[id]["machine"] + "_" + nodes[id]["process"];
    }
    return index;
}

function getMongosServers( config , nodes , index ){
    config.mongos.find().forEach( function(doc) {
	var id = index;
	index++;
	nodes[id] = {};
	nodes[id]["hostName"] = doc["_id"];
	nodes[id]["machine"] = ""; //to be expanded later
	nodes[id]["process"] = "mongos";
	nodes[id]["role"] = "mongos";	
    	nodes[id]["errors"] = new Array();
	nodes[id]["warnings"] = new Array();
	nodes[id]["key"] = nodes[id]["hostName"] + "_" + nodes[id]["machine"] + "_" + nodes[id]["process"];
    });
    return index;
}

function getShardServers( configDB , nodes , index ){
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
		nodes[id]["errors"] = new Array();
		nodes[id]["warnings"] = new Array();
		nodes[id]["shardName"] = shardName;
		nodes[id]["key"] = nodes[id]["hostName"] + "_" + nodes[id]["machine"] + "_" + nodes[id]["process"];
		if(i == 0)
		    nodes[id]["role"] = "primary";	
		else
		    nodes[id]["role"] = "secondary";
		try{
		    var conn = new Mongo( hosts[i] );
		    var admin = conn.getDB("admin");
		    var masterStats = admin.runCommand( { ismaster : 1 } );
		    nodes[id]["replSetName"] = masterStats["setName"];
		    //add check for no config server noted
		}
		catch(e){
		    nodes[id]["replSetName"] = "undefined";
		    nodes[id]["warnings"].push( ERR["NO_REPL_SET_NAME_NOTED"] );
		}  
	    }	
	 }
        
	//if the shard has a standalone mongod instance 
	else{
	    var newNode = {};
	    newNode[id] = index;
	    index++;
	    newNode["hostName"] = doc["host"] 
	    newNode["machine"] = ""; //to be expanded later
	    newNode["process"] = "mongod";
	    nodes[id]["key"] = nodes[id]["hostName"] + "_" + nodes[id]["machine"] + "_" + nodes[id]["process"];
	    nodes.push(newNode);	
	}
    });
    return index;	
}

function buildIdMap( nodes , idMap ){
    for ( var curr in nodes ){
	idMap[ nodes[curr]["hostName"] 
	    + "_" + nodes[curr]["machine"] 
	    + "_" + nodes[curr]["process"] ] 
	    = curr;
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
    var edgeStats = {};
    for( var srcName in history["allNodes"] ){
	edgeStats[ srcName ] = {};
	for( var tgtName in history["allNodes"]){
	    if( srcName != tgtName){ 
		edgeStats[ srcName ][ tgtName ] = {
		    "numPingAttempts" : 0,
		    "numSuccessful" : 0,
		    "numFailed" : 0,	
		    "totalSocketExceptions" : 0,
		    "percentageIsConnected" :0,
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
		    if( currEdges[ src ][ tgt ] != null ){ //if edge existed in this snapshot
			edgeStats[ srcName ][ tgtName ]["numPingAttempts"]++;
			edgeStats[ srcName ][ tgtName ]["totalSocketExceptions"] 
			    = parseInt(edgeStats[ srcName ][ tgtName ]["totalSocketExceptions"])
			    + parseInt(currEdges[ src ][ tgt ]["totalSocketExceptions"]);
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
		edgeStats[ srcName ][ tgtName ]["percentageIsConnected"] = 100  
		    * parseFloat( edgeStats[ srcName ][ tgtName ]["numSuccessful"]) 
		    / parseFloat( edgeStats[ srcName ][ tgtName ]["numPingAttempts"]);	
	    }
	}
    }
    // standard deviation
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
    var count=0;
    for (var moment in history["snapshots"])
	count++;
    if(count < 1)
	print("Not enough snapshots to calculate statistics. Please ping cluster at least once.");
    else
	printjson(edgeStats); 
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
	print("Not enough snapshots to calculate deltas. Please ping cluster at least twice.");
    else
	printjson(deltas); 
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


