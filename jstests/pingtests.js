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

    var nodes = {};   
    var edges = {};
    var idMap = {};
    var index = 0; 

    index = getShardServers( configDB , nodes , index , idMap);
    index = getMongosServers( configDB , nodes , index , idMap);
    index = getConfigServers( configDB , nodes , index , idMap);
  
    buildGraph( nodes , edges );   
    buildIdMap( nodes , idMap );

    var currDate = new Date();
    var currTime = currDate.toUTCString(); 
    saveSnapshot( currTime , nodes , edges , idMap , envErrors , envWarnings);

    var diagnosis = diagnose( nodes , edges );
//    var userView = buildUserView( diagnosis , verbosity ); 

//    printjson( diagnosis );
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
    "MISSING_REQ_CONNECTION" : "Missing required connection between ",
    "MISSING_REC_CONNECTION" : "Missing recommended connection between ",
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
    var srcRole;
    var tgtRole;
    if( nodes[src]["process"] == "mongos")
	srcRole = "mongos";
    else
	srcRole = nodes[src]["role"];
    if( nodes[tgt]["process"] == "mongos")
	tgtRole = "mongos";
    else
	tgtRole = nodes[tgt]["role"]; 
    return reqConnChart[ srcRole ][ tgtRole ];
}

function isRecConn( src , tgt , nodes ){
    var srcRole;
    var tgtRole;
    if( nodes[src]["process"] == "mongos") srcRole = "mongos";
    else
	srcRole = nodes[src]["role"];
    if( nodes[tgt]["process"] == "mongos")
	tgtRole = "mongos";
    else
	tgtRole = nodes[tgt]["role"]; 
    return recConnChart[ srcRole ][ tgtRole ];
}

function diagnose( nodes , edges ){
    var diagnosis = {};
    var errors = {};
    var warnings = {};

    //check edges
    for( var src in edges ){
	for( var tgt in edges[src] ){
	    if( edges[ src ][ tgt ]["isConnected"] == false){
		if( isReqConn( src , tgt , nodes ) )
		    errors.push( ERR["MISSING_REQ_CONNECTION"] 
			+ nodes[src]["hostName"] + ", " + nodes[tgt]["hostName"]); 
		if( isRecConn( src , tgt , nodes ) )
		    warnings.push( ERR["MISSING_REC_CONNECTION"] 
			+ nodes[src]["hostName"] + ", " + nodes[tgt]["hostName"]); 
	    }
	}
    }

    //check replica sets


    diagnosis["errors"] = errors;
    diagnosis["warnings"] = warnings;
    
    return diagnosis;
    });

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
function buildGraph( nodes , edges ){
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
		    newEdge["numSocketExceptions"] = 
			pingInfo[ nodes[tgtNode]["hostName"] ]["numSocketExceptions"]; 
		    edges[ srcNode ][ tgtNode ] = newEdge;	    
		}
		catch(e){
	//	    edges[ srcNode[id] ][ tgtNode[id] ] = 
	//		ERR["CLIENT_CONN_ERR"] + tgtNode["hostName"];
		   edges[ srcNode ][ tgtNode ] = e;
		} 
	    }
	}
    }
}

function getConfigServers( config , nodes , index ){

    configSvr = db.configDB.getShardVersion()["configServer"];
    if(configSvr != null && configSvr != ""){
    	var id = index;
	index++;
	nodes[id] = {};
	nodes[id]["hostName"] = doc["_id"];
	nodes[id]["machine"] = ""; //to be expanded later
	nodes[id]["process"] = "mongod";	
    	nodes[id]["errors"] = new Array();
	nodes[id]["warnings"] = new Array();
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
    	nodes[id]["errors"] = new Array();
	nodes[id]["warnings"] = new Array();
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

function showAllNodes(){ 
    printjson( history["allNodes"] ); }
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
	if( history["allNodes"][ curr ] == null)
	    history["allNodes"][ curr ] = "alive"; 
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
		    "numSocketExceptions" : 0,
		    "percentageIsConnected" :0,
		    "maxPingTimeMicrosecs" : null,
		    "minPingTimeMicrosecs" : null,
		    "sumPingTimeMicrosecs" : 0,
		    "avgPingTimeMicrosecs" : 0,
		    "pingTimeStdDeviation" : 0,
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
			edgeStats[ srcName ][ tgtName ]["numSocketExceptions"] 
			    = parseInt(edgeStats[ srcName ][ tgtName ]["numSocketExceptions"])
			    + parseInt(currEdges[ src ][ tgt ]["numSocketExceptions"]);
			if( currEdges[ src ][ tgt ]["isConnected"] == true){
			    edgeStats[ srcName ][ tgtName ]["numSuccessful"]++;
			    var pingTime = currEdges[ src ][ tgt ]["pingTimeMicrosecs"];
			    if( edgeStats[ srcName ][ tgtName ]["maxPingTimeMicrosecs"] == null 
				|| pingTime > edgeStats[ srcName ][ tgtName ]["maxPingTimeMicrosecs"])
				edgeStats[ srcName ][ tgtName ]["maxPingTimeMicrosecs"] = pingTime;	
			    if( edgeStats[ srcName ][ tgtName ]["minPingTimeMicrosecs"] == null 
				|| pingTime < edgeStats[ srcName ][ tgtName ]["minPingTimeMicrosecs"])
				edgeStats[ srcName ][ tgtName ]["minPingTimeMicrosecs"] = pingTime;	
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
		if( srcName != tgtName){	
		    var src = history["snapshots"][moment]["idMap"][ srcName ];	
		    var tgt = history["snapshots"][moment]["idMap"][ tgtName ];
		    if( currEdges[ src ][ tgt ] != null ){
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
	currSnapshot = history["snapshots"][moment];	
	if(count != 0){
	    var prevSnapshot = history["snapshots"][prevMoment];
	    deltas[ moment ] = {};
	    deltas[ moment ]["prevTime"] = prevMoment;
	    deltas[ moment ]["newErrors"] = new Array();
	    deltas[ moment ]["newWarnings"] = new Array();
	    deltas[ moment ]["newNodes"] = new Array();
	    deltas[ moment ]["removedErrors"] = new Array();
	    deltas[ moment ]["removedWarnings"] = new Array();
	    deltas[ moment ]["removedNodes"] = new Array(); 
	    for( var currNode in currSnapshot["idMap"] )
		if( prevSnapshot["idMap"][ currNode ] == null )
		    deltas[ moment ]["newNodes"].push( currNode );
	    for( var prevNode in prevSnapshot["idMap"] )
		if( currSnapshot["idMap"][ prevNode ] == null )
		    deltas[ moment ]["removedNodes"].push( prevNode );	    	
	}
	prevMoment = moment; 
	count++;  
    }		
    if(count < 2)
	print("Not enough snapshots to calculate deltas. Please ping cluster at least twice.");
    else
	printjson(deltas); 
}






