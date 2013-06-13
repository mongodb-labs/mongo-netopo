
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
	s = new ShardingTest( {name : "shard1", rs : { nodes : 3 }} , 3 , 0 , 3 );
	return s;			

}

// Takes in a connection of the form "host:port"
// and returns a complete categorized graph of the connections
// associated with this node
function pingShardedCluster( host ) {

	conn = new Mongo( host );
	var config = conn.getDB("config");

	shardPrimaries = new Array();
	shardSecondaries = new Array();
	mongosHosts = new Array();
	configServers = new Array();
	
	config.shards.find().forEach( function(curr_doc) {
		if((curr_pos = curr_doc["host"].indexOf("/")) != -1)
		{
			curr_pos++;
			var next_host;
			var next_comma;
			var count = 0;	
			while((next_comma = curr_doc["host"].indexOf(",", curr_pos)) != -1)
			{		
				next_host = curr_doc["host"].substring(curr_pos, next_comma);
				if(count == 0)	
					shardPrimaries.push(next_host);
				else
					shardSecondaries.push(next_host);
				curr_pos = next_comma + 1;
				count++;
			}		
			//the last one isn't ended by a comma	
			next_host = curr_doc["host"].substring(curr_pos);
			shardSecondaries.push(next_host);
			curr_pos = next_comma + 1;
		}
		else
			shardHosts.push(curr_doc["host"]);
	});	

	config.mongos.find().forEach( function(curr_doc) {
		mongosHosts.push(curr_doc["_id"]);
	});

	diagnosis = {};

	diagnosis["mongos servers to mongos servers"] = {"type" : "not required" , "status" : check( mongosHosts , mongosHosts )};	
	diagnosis["mongos servers to shard primaries"] = {"type" : "required" , "status" : check( mongosHosts , shardPrimaries )};
	diagnosis["mongos servers to shard secondaries"] = {"type" : "recommended" , "status" : check( mongosHosts , shardSecondaries )};
//	diagnosis["mongos servers to config server"] = {"type" : "required" , "status" : check( mongosHosts, configServers )};

	diagnosis["shard primaries to shard primaries"] = {"type" : "required" , "status" : check( shardPrimaries, shardPrimaries )};
	diagnosis["shard primaries to shard secondaries"] = {"type" : "recommended" , "status" : check( shardPrimaries, shardSecondaries )};
	diagnosis["shard primaries to mongos servers"] = {"type" : "required" , "status" : check( shardPrimaries, mongosHosts )};
//	diagnosis["shard primaries to config servers"] = {"type" : "required" , "status" : check( shardPrimaries, configServers )};

	diagnosis["shard secondaries to shard secondaries"] = {"type" : "recommended" , "status" : check( shardSecondaries , shardSecondaries )};
	diagnosis["shard secondaries to shard primaries"] = {"type" : "recommended" , "status" : check( shardSecondaries , shardPrimaries )};
	diagnosis["shard secondaries to mongos servers"] = {"type" : "recommended" , "status" : check( shardSecondaries , mongosHosts )};  
//	diagnosis["shard secondaries to config servers"] = {"type" : "recommended" , "status" : check( shardSecondaries , convigServers )};

/*
	diagnosis["config servers to config servers"] = {"type" : "required" , "status" : check( configServers , configServers )};
	diagnosis["config servers to mongos servers"] = {"type" : "required" , "status" : check( configServers, mongosHosts )};
	diagnosis["config servers to shard primaries"] = {"type" : "required" , "status" : check( configServers, shardPrimaries )};
	diagnosis["config servers to shard secondaries"] = {"type" : "recommended" , "status" : check( configServers, shardSecondaries )};
*/

	//Check all replica sets for inner connectivity
	diagnosis["inner connectivity of replica sets"] = {"type" : "required"};	
	for(var i=0; i<shardPrimaries.length; i++)
	{
		conn = new Mongo( shardPrimaries[i] );
		db = conn.getDB("admin");
		
		//might want other stats in the future
		masterStats = db.runCommand( { ismaster : 1 } );
	
		setList = masterStats["hosts"];
		setList.push( shardPrimaries[i] );
		diagnosis["inner connectivity of replica sets"][shardPrimaries[i]] = check( setList , setList );
	}	
	
	printjson(diagnosis);

}

function check( fromList, toList )
{
	from = {}; 
	for(var i=0; i<fromList.length; i++)
	{
		var curr_conn = new Mongo( fromList[i] );
		curr_db = curr_conn.getDB("admin");
		pingresults = curr_db.runCommand( { "ping" : 1, hosts : toList } ) ;	
		to = {};
		for(var j=0; j<toList.length; j++)
		{
			if(pingresults["results"][toList[j]]["connection message"] == null)
				to[toList[j]] = "ok";
			else
				to[toList[j]] = "err : " + pingresults["results"][toList[j]]["connection message"];
		}
		from[fromList[i]] = to;
	} 
	return from;	
} 


