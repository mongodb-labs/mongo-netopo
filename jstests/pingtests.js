var historicalData = new Array();

// Sets up and starts a replica set of three nodes to
// be used to test other functions on
// Returns a reference to the replica set
function createReplSet() {

    // Replica set testing API
    load("jstests/replsets/rslib.js");

    var replTest = new ReplSetTest( {name : 'testSet', nodes : 3});
    var nodes = replTest.startSet();
    replTest.initiate();

    return replTest;

}

// Takes in a connection of the form "host:port"
// and returns a complete graph of the connections
// associated with this node
/*function pingReplSet( host ) {

  graph["inner connectivity of replica sets"] = {"type" : "required"};	

  conn = new Mongo( primaryServers[i] );
  db = conn.getDB("admin");

//might want other stats in the future
masterStats = db.runCommand( { ismaster : 1 } );

if(masterStats["hosts"])	
{
setList = masterStats["hosts"];
setList.push( conn );
graph["inner connectivity of replica set"][] = check( setList , setList );
}
else
print}
}	


}
 */


// Sets up and starts a sharded cluster of three nodes to
// be used to test other functions on
// returns a reference to the cluster
function createShardedCluster() {

    // ShardingTest = function( testName, numShards, verboseLevel, numMongos, otherParams )
    // testName is the cluster name	
    //s = new ShardingTest( "shard1" , 3 , 0 , 3 );
    s = new ShardingTest( {name : "shard1" , rs: { nodes : 3 } } , 3 , 0 , 3 );
    return s;			

}

// Takes in a connection of the form "host:port"  and returns a complete graph of the connections
// associated with this node
function pingShardedCluster( host ) {

    var graph = {};

    var curr_date = new Date();
    graph["currentTime"] = curr_date.toUTCString(); 

    var conn = new Mongo( host );
    var config = conn.getDB("config");

    
    // to be used in output 
    // type is either "primary" , "secondary" , "mongos" , or "config"
    var allServersWithType = new Array();
    // to be passed as the array of servers to the ping command 
    var allServers = new Array();
   
    getShardServers( config , allServersWithType , allServers);
    getMongosServers( config , allServersWithType , allServers);
    getConfigServers( config , allServersWithType , allServers);

    var connectionsObj = {};
  
    allServersWithType.map( function(server) { 
	var hostString = server["host"];	
	connectionsObj[hostString] = {};		
	connectionsObj[hostString]["type"] = server["type"];
	var curr_conn = new Mongo( hostString );
	var curr_db = curr_conn.getDB("admin");
	connectionsObj[hostString]["outConnections"] = 
	    curr_db.runCommand( { "ping" : 1, hosts : allServers } ) ;	
    });
    
    graph["allConnections"] = connectionsObj;
   
    saveSnapshot( graph ); 
    printjson( graph );
    
}

function getConfigServers( config, allServersWithType , allServers){

    //issues with getShardVersion : output :

/*  >db.config.getShardVersion()
    {
	"configServer" : "",
	"global" : Timestamp(0, 0),
	"inShardedMode" : false,
	"mine" : Timestamp(0, 0),
	"ok" : 1
    }  
*/



    //however, printShardingStatus() through mongos reports sharding...

/*
--- Sharding Status --- 
  sharding version: {
	"_id" : 1,
	"version" : 3,
	"minCompatibleVersion" : 3,
	"currentVersion" : 4,
	"clusterId" : ObjectId("51bf1b093bf1c7aff6d00fae")
}
  shards:
	{  "_id" : "shard1-rs0",  "host" : "shard1-rs0/localhost:31100,localhost:31101,localhost:31102" }
	{  "_id" : "shard1-rs1",  "host" : "shard1-rs1/localhost:31200,localhost:31201,localhost:31202" }
  databases:
	{  "_id" : "admin",  "partitioned" : false,  "primary" : "config" }
	{  "_id" : "test",  "partitioned" : false,  "primary" : "shard1-rs0" }
*/



}

function getMongosServers( config , allServersWithType , allServers){
    config.mongos.find().forEach( function(curr_doc) {
	allServers.push( curr_doc["_id"] ); 
	addServer( allServersWithType , curr_doc["_id"] , "mongos" );
    });
}

function getShardServers( config , allServersWithType , allServers){
    // string parsing for shard servers
    // originally in format "shard-01/lcalhost:30000,localhost:30001,localhost:30002"	
    config.shards.find().forEach( function(curr_doc) {
        if((startPos = curr_doc["host"].indexOf("/")) != -1) {
	    var hosts = curr_doc["host"].substring( startPos + 1 ).split(",");
	    for(var i=0; i<hosts.length; i++) {
		if(i == 0){
		    allServers.push(hosts[i]); 
		    addServer( allServersWithType , hosts[i] , "primary" );	
		}	
		else{
		    allServers.push(hosts[i]);	
		    addServer( allServersWithType , hosts[i] , "secondary" );	
		}
	    }	
	 }
        else{
	    allServers.push( curr_doc["host"] );
	    addServer( allServersWithType , curr_doc["host"] , "primary" );	
	}
    });	
}

function addServer( array , host , type ){
    var toAdd = {}
    toAdd["host"] = host;
    toAdd["type"] = type;
    array.push(toAdd); 
}

function saveSnapshot( graph ){
    historicalData.push( graph ); 
}

function showHistoricalData(){
    var times = new Array();
    historicalData.map( function(snapshot){
	times.push(snapshot["currentTime"]);
    }); 
    printjson( times );
    //printjson( historicalData );
}

