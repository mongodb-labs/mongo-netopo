load("jstests/replsets/rslib.js");
doTest = function( signal ) {

	// Set up a replica set with three nodes and have each node ping
	// every other node (complete graph) and return 
	// whether each ping was received or not.

	// Replica set testing API
	// Create a new replica set test. Specify a set name and
	// the number of nodes.
	var replTest = new ReplSetTest( {name: 'testSet', nodes: 3} );

	// Call startSet() to start each mongod in the replica set
	// this returns a list of nodes
	var nodes = replTest.startSet();
	
	// Call initiate() to send the replSetInitiate command
	// This will wait for initiation
	replTest.initiate();

	// Call getMaster to return a reference to the node that's 
	// been elected master.
	var master = replTest.getMaster();

	// Call getDB on database "admin" to be able to 
	// run database commands on the master mongod instance
	var accessDB = "admin";	
	var db_master = master.getDB(accessDB);

	// Store the list of hosts that the master is connected to
	// in its replica set
	masterStats = db_master.runCommand({ismaster : 1});
	hostList = masterStats["hosts"];

	// For each node in the replica set, ping all the nodes in
	// the set (including itself). Also print whether the node
	// is a primary or a secondary
	print("Ping Results:");
	var curr_host;	
	var length = hostList.length;
	for(var i=0; i<length; i++)
	{
		curr_host = nodes[i].getDB(accessDB);
		curr_stats = curr_host.runCommand({isMaster: 1});	
		if(curr_stats.me == curr_stats.primary)
			print("Primary:");
		else
			print("Secondary:");	
		printjson( curr_host.runCommand( { "ping" : 1, hosts : hostList} ) );
	}
	print("\n\n\n\n\n\n\n\n\n");
	printjson(hostList);
	print("\n\n\n\n\n\n\n\n\n");
	
	replTest.stopSet( signal );
}

doTest( 15 );
print("pingReplSet.js SUCCESS");
