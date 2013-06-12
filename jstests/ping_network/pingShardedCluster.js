// Set up a sharded cluster with three nodes and have each node
// ping every other node (complete graph) and return whether
// each ping was received or not.

	// Sharded cluster testing API
	// Create a new test sharded cluster. Specify a cluster name and
	// the number of nodes.
	s = new ShardingTest( "shard1" , 3 );

	// Call getDB on database "admin" to be able to run database
	// commands on the cluster
	var accessDB = "admin";
//	var db_master = s.getDB( accessDB );

	// Store the list of hosts in this cluster
	// Format: "host:port"
	hostList = s.getConnNames();
	// Format: "connection to host:port"
	nodes = s._connections;
	
	// For each node in the replica set, ping all the nodes in the
	// set (including itself). 
	print("Ping Results: ");	
	var curr_host;
	var length = hostList.length;
	for(var i=0; i<length; i++)
	{
		curr_host = nodes[i].getDB(accessDB);
	//	masterStats = db_master.runCommand({ismaster : 1});
	//	printjson(masterStats);
		if(curr_host.runCommand({ismaster : 1}))
			print("\n\n\n\nmaster\n\n\n\n\n");	
		printjson( curr_host.runCommand( { "ping" : 1, hosts: hostList } ) );
	}	

	s.stop();




