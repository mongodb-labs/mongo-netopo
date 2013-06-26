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

    var nodes = new Array();   
    var edges = {};
    var index = 0; 

    index = getShardServers( configDB , nodes , index );
    index = getMongosServers( configDB , nodes , index );
    index = getConfigServers( configDB , nodes , index );

    recordNewNodes( nodes );
   
    buildGraph( nodes , edges );   

    var graph = {};
    var curr_date = new Date();
    saveSnapshot( curr_date.toUTCString() , nodes , edges);

//    var diagnosis = makeDiagnosis( nodes , edges );
//    var userView = buildUserView( diagnosis , verbosity ); 

//    printjson( diagnosis );
//    printjson( userView );

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

function makeDiagnosis( nodes , edges ){
    var diagnosis = {};
    diagnosis["mongos"] = new Array();
    diagnosis["shards"] = new Array();
    diagnosis["config"] = new Array();

    nodes.map( function(node){
    	if( node["process"] == "mongos"){
	    var newNode = {};
	    newNode["hostName"] = node["hostName"];
	    newNode["status"] = diagnose( node[id] , nodes , edges ); 
	    diagnosis["mongos"].push(newNode);     
	}	
	else if( node["process"] == "mongod" && node["role"] == "config" ){
	    var newNode = {};
	    newNode["hostName"] = node["hostName"];
	    newNode["status"] = diagnose( node[id] , nodes , edges ); 
	    diagnosis["config"].push(newNode);     
	}	
	else if( node["process"] == "mongod" && 
	    ( node["role"] == "primary" || node["role"] == "secondary" ) ){
	    var shardIndex = indexOfJSONDoc(diagnosis["shards"],"shardName",node["replSetName"]);	
	    if( shardIndex < 0 ){ 
		var newShard = {}; 
		newShard["shardName"] = node["replSetName"];
		newShard["status"] = {};	
		newShard["primary"] = new Array();
		newShard["secondary"] = new Array();
		var newNode = {};
		newNode["hostName"] = node["hostName"];
		newNode["status"] = diagnose( node[id] , nodes , edges );	
		newShard[ node["role"] ].push(newNode);	
		diagnosis["shards"].push(newShard);
	    }
	    else{
		var newNode = {};
		newNode["hostName"] = node["hostName"];
		newNode["status"] = diagnose( node[id] , nodes , edges );	
		diagnosis["shards"][shardIndex][ node["role"] ].push(newNode);
		 
	    }	
	}
    });
  
    diagnosis["shards"].map( function(node){
	node["status"] = diagnoseShard( node ); 
    }); 
    
    return diagnosis;
}

function indexOfJSONDoc( array , idType , myId ){
    for(var i=0; i<array.length; i++){
	if( array[i][idType] == myId)
	    return i;
    }
    return -1;
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
    "MISSING_REQ_CONNECTION" : "Missing required connection at ",
    "MISSING_REC_CONNECTION" : "Missing recommended connection at ",
    "CLIENT_CONN_ERROR" : "Client unable to make connection to "
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

function getRoleFromId( myID , nodes ){
    for(var i=0; i<nodes.length; i++){
	if( nodes[i][ id ] == myID){
	    if( nodes[i]["process"] == "mongos")
		return "mongos";
	    else
		return nodes[i]["role"];
	} 
    }
}

function getHostFromId( myID , nodes ){
    for(var i=0; i<nodes.length; i++){
	if( nodes[i][ id ] == myID )
	    return nodes[i]["hostName"];
    } 
}

function isReqConn( srcRole , tgtRole ){
    return reqConnChart[ srcRole ][ tgtRole ];
}

function isRecConn( srcRole , tgtRole ){
    return recConnChart[ srcRole ][ tgtRole ];
}

function diagnose( srcId , nodes , edges  ){
  
    var errors = new Array();
    var warnings = new Array();

    var srcRole = getRoleFromId( srcId , nodes );
    edges.map( function(edge){
        if(edge[src] == srcId){
	    var tgtRole = getRoleFromId( edge[tgt] , nodes );	
	    if( edge["isConnected"] == false){
		if( isReqConn( srcRole , tgtRole ) )
		    errors.push( ERR["MISSING_REQ_CONNECTION"] + getHostFromId( edge[tgt] , nodes ));
		if( isRecConn( srcRole , tgtRole ) )
		    warnings.push( ERR["MISSING_REC_CONNECTION"] + getHostFromId(edge[tgt] , nodes));
	    }	
	}
    });

    var	memberStatus = {};
    if( warnings.length == 0 && errors.length == 0)
	memberStatus["ok"] = 1; 
    else
	memberStatus["ok"] = 0;
    memberStatus["warnings"] = warnings;
    memberStatus["errors"] = errors;
 
    return memberStatus;	
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
    nodes.map( function(srcNode) {
	edges[ srcNode[id] ] = {}; 
	nodes.map( function(tgtNode) {	
	    if(srcNode[id] != tgtNode[id]){ 
		var newEdge = {};
		try{	
		    var conn = new Mongo( srcNode["hostName"] );
		    var admin = conn.getDB("admin");
		    var pingInfo = admin.runCommand( { "ping" : 1, hosts : [tgtNode["hostName"]] } );
		    if( pingInfo[ tgtNode["hostName"] ][ "isConnected" ] == false){
			newEdge["isConnected"] = false;
			newEdge["errmsg"] = pingInfo[ tgtNode["hostName"] ][ "errmsg" ];	
		    }
		    else{
			newEdge["isConnected"] = true;
			newEdge["pingTimeMicrosecs"] = pingInfo[ tgtNode["hostName"] ][ "pingTimeMicrosecs" ]; 
			//more ping info can be added here later 
		    }
		    edges[ srcNode[id] ][ tgtNode[id] ] = newEdge;	    
		}
		catch(e){
		   // edges[ srcNode[id] ][ tgtNode[id] ] = ERR["CLIENT_CONN_ERR"] + tgtNode["hostName"];
		   // edges[ srcNode[id] ][ tgtNode[id] ] = e;
		   edges[ srcNode[id] ][ tgtNode[id] ] = null;
		} 
	    }
	});
    });
}
/*
//Although the {ping} function takes an array of hosts to ping, this function
//pings each node one at a time (passes an array of one element to {ping})
//because the edges array is to be built in a format of source-target pairs
//rather than a source-[list of targets] format
function oldbuildGraph( nodes , edges ){
    nodes.map( function(srcNode) {
	nodes.map( function(tgtNode) {	
	    if(srcNode[id] != tgtNode[id]){ 
		var newEdge = {};
		newEdge[src] = srcNode[id];
		newEdge[tgt] = tgtNode[id];
		try{	
		    var conn = new Mongo( srcNode["hostName"] );
		    var admin = conn.getDB("admin");
		    var pingInfo = admin.runCommand( { "ping" : 1, hosts : [tgtNode["hostName"]] } );
		    if( pingInfo[ tgtNode["hostName"] ][ "isConnected" ] == false){
			newEdge["isConnected"] = false;
			newEdge["errmsg"] = pingInfo[ tgtNode["hostName"] ][ "errmsg" ];	
		    }
		    else{
			newEdge["isConnected"] = true;
			newEdge["pingTimeMicrosecs"] = pingInfo[ tgtNode["hostName"] ][ "pingTimeMicrosecs" ]; 
			//more ping info can be added here later 
		    }
		    edges.push(newEdge);
		}
		catch(e){
		    // these are fundamentally WRONG. it's a client connection error, not ping error 
		    // newEdge["isConnected"] = false;
		    // newEdge["errmsg"] = e;	
		} 
	    }
	});
    });
}
*/

function getConfigServers( config , nodes , index ){

    configSvr = db.configDB.getShardVersion()["configServer"];
    if(configSvr != null && configSvr != ""){
	var newNode = {};
	newNode[id] = index;
	index++;
	newNode["hostName"] = configSvr;
	newNode["machine"] = ""; //to be expanded later
	newNode["process"] = "mongod";
	nodes.push(newNode);
    }
    return index;
}

function getMongosServers( config , nodes , index ){
    config.mongos.find().forEach( function(doc) {
	var newNode = {};
	newNode[id] = index;
	index++;
	newNode["hostName"] = doc["_id"];
	newNode["machine"] = ""; //to be expanded later
	newNode["process"] = "mongos";	
	newNode["role"] = "mongos";	
	nodes.push(newNode); 
    });
    return index;
}

function getShardServers( configDB , nodes , index ){
    
        configDB.shards.find().forEach( function(doc) {
	
	// if the shard is a replica set 
	// do string parsing for shard servers
	// originally in format "shard-01/lcalhost:30000,localhost:30001,localhost:30002"	
	if( (startPos = doc["host"].indexOf("/") ) != -1) {
	    var hosts = doc["host"].substring( startPos + 1 ).split(",");
	    for(var i=0; i<hosts.length; i++) {
		var newNode = {};
		newNode[id] = index;
		index++;
		newNode["hostName"] = hosts[i];
		newNode["machine"] = ""; //to be expanded later	
		newNode["process"] = "mongod"; 
		if(i == 0)
		    newNode["role"] = "primary";	
		else
		    newNode["role"] = "secondary";
		try{
		    var conn = new Mongo( hosts[i] );
		    var admin = conn.getDB("admin");
		    var masterStats = admin.runCommand( { ismaster : 1 } );
		    newNode["replSetName"] = masterStats["setName"];
		    nodes.push(newNode); 
		}
		catch(e){
		    newNode["replSetName"] = "undefined";
		    nodes.push(newNode);	 
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

function recordNewNodes( nodes ){
    nodes.map( function(node){
	if( history["allNodes"][ node[id] ] == null)
	    history["allNodes"][ node[id] ] = node["hostName"] + "_" + node["process"] + "_" + node["machineName"];
    });
}

function showAllNodes(){ printjson( history["allNodes"] ); }

function saveSnapshot( time , nodes , edges ){
    history["snapshots"][ time ] = { "nodes" : nodes , "edges" : edges }; 
}

function showHistory(){
    var times = new Array();
    history["snapshots"].map( function(snapshot){
	times.push(snapshot["currentTime"]);
    }); 
    printjson( times );
    //printjson( history );
}

function calculateStats(){

    var edgeStats = {};
    for( var i in history["allNodes"] ){
	edgeStats[i] = {};
	for( var j in history["allNodes"]){
	    edgeStats[i][j] = {
		"numPingAttempts" : 0,
		"numSuccessful" : 0,
		"numFailed" : 0,	
		"numSocketExceptions" : 0,
		"percentageIsConnected" :0,
		"maxPingTimeMicrosecs" : 0,
		"minPingTimeMicrosecs" : 999999999999,
		"sumPingTimeMicrosecs" : 0,
		"avgPingTimeMicrosecs" : 0,
		"pingTimeStdDeviation" : 0,
		"subtractMeanSquaredSum" : 0
	    };
	}	
    }

    // max ping time, min ping time, num ping attempts, num successful, num failed
    for(var moment in history["snapshots"]){	
	var snapshot = history["snapshots"][moment];	
	for( var i in history["allNodes"] ){
	    for( var j in history["allNodes"] ){
    		if( snapshot["edges"][i][j] != null ){
		    edgeStats[i][j]["numPingAttempts"]++;
		    if( snapshot["edges"][i][j]["isConnected"] )
			edgeStats[i][j]["numSuccessful"]++;
		    else
			edgeStats[i][j]["numFailed"]++; 
		    var pingTime = snapshot["edges"][i][j]["pingTimeMicrosecs"]; 
		    if( pingTime > edgeStats[i][j]["maxPingTimeMicrosecs"])
			edgeStats[i][j]["maxPingTimeMicrosecs"] = pingTime;	
		    if( pingTime < edgeStats[i][j]["minPingTimeMicrosecs"])
			edgeStats[i][j]["minPingTimeMicrosecs"] = pingTime;	
		    edgeStats[i][j]["sumPingTimeMicrosecs"] = 
			parseInt(edgeStats[i][j]["sumPingTimeMicrosecs"]) + parseInt(pingTime);	
		}
	    }
	}
    }	

    // avg ping time and percentage connected 
    for( var i in history["allNodes"] ){
	for( var j in history["allNodes"] ){
	    edgeStats[i][j]["avgPingTimeMicrosecs"] = 
		parseFloat(edgeStats[i][j]["sumPingTimeMicrosecs"]) / parseFloat(edgeStats[i][j]["numSuccessful"]);	
	    edgeStats[i][j]["percentageIsConnected"] = 
		100 * parseFloat(edgeStats[i][j]["numSuccessful"]) / parseFloat(edgeStats[i][j]["numPingAttempts"]);	
	}
    }

    // standard deviation
    for(var moment in history["snapshots"]){	
	var snapshot = history["snapshots"][moment];	
	for( var i in history["allNodes"] ){
	    for( var j in history["allNodes"] ){
		if( snapshot["edges"][i][j] != null ){
		    edgeStats[i][j]["subtractMeanSquaredSum"] = parseFloat(edgeStats[i][j]["subtractMeanSquaredSum"])
			+ (parseFloat(snapshot["edges"][i][j]["pingTimeMicrosecs"]) 
			    - parseFloat(edgeStats[i][j]["avgPingTimeMicrosecs"])) 
			* (parseFloat(snapshot["edges"][i][j]["pingTimeMicrosecs"]) 
			    - parseFloat(edgeStats[i][j]["avgPingTimeMicrosecs"])); 
		} 
	    }
	}
    }
    for( var i in history["allNodes"] ){
	for( var j in history["allNodes"] ){
	    edgeStats[i][j]["pingTimeStdDeviation"] = Math.sqrt(parseFloat(edgeStats[i][j]["subtractMeanSquaredSum"]) / parseFloat(edgeStats[i][j]["numSuccessful"]));  
	    delete edgeStats[i][j]["subtractMeanSquaredSum"];	
	    delete edgeStats[i][j]["sumPingTimeMicrosecs"]; 
	}
    }


    printjson(edgeStats[5][0]);

}
