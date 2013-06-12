
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
function pingReplSet( host ) {

	conn = new Mongo( host );
	var db = conn.getDB("admin");

	//might want other stats in the future
	masterStats = db.runCommand( { ismaster : 1 } );
	
	hostList = masterStats["hosts"];

	for(var i=0; i<hostList.length; i++)
	{
		var curr_conn = new Mongo( hostList[i] );
		curr_db = curr_conn.getDB("admin");
		curr_stats = curr_db.runCommand({isMaster: 1});
		if(curr_stats.me == curr_stats.primary)
			print("Primary:");
		else
			print("Secondary:");
		printjson( curr_db.runCommand( { "ping" : 1, hosts : hostList } ) ); 
	}	

}



// Sets up and starts a sharded cluster of three nodes to
// be used to test other functions on
// returns a reference to the cluster
function createShardedCluster() {

	// ShardingTest = function( testName, numShards, verboseLevel, numMongos, otherParams )
	// testName is the cluster name	
	s = new ShardingTest( "shard1" , 3 , 0 , 3 );
	return s;			

}


// Takes in a connection of the form "host:port"
// and returns a complete graph of the connections
// associated with this node
function pingShardedCluster( host ) {

	conn = new Mongo( host );
	var config = conn.getDB("config");

	shardHosts = new Array();
	mongosHosts = new Array();

	config.shards.find().forEach( function(curr_doc) {
		shardHosts.push(curr_doc["host"]);
	});	

	config.mongos.find().forEach( function(curr_doc) {
		mongosHosts.push(curr_doc["_id"]);
	});

	hostList =mongosHosts.concat( shardHosts );

	for(var i=0; i<hostList.length; i++)
	{
		var curr_conn = new Mongo( hostList[i] );
		curr_db = curr_conn.getDB("admin");
		curr_stats = curr_db.runCommand({isMaster: 1});
		if(curr_stats.me == curr_stats.primary)
			print("Primary:");
		else
			print("Secondary:");
		printjson( curr_db.runCommand( { "ping" : 1, hosts : hostList } ) ); 
	}	


}
