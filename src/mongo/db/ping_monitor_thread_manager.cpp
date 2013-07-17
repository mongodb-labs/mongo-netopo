// ping_monitor_thread_manager.cpp

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

#include "ping_monitor_thread_manager.h"

#include "mongo/client/connpool.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/is_master.h"

namespace mongo {

    // PingMonitorThreadManager

    HostAndPort PingMonitorThreadManager::self;
    bool PingMonitorThreadManager::selfSet;

    // map of all targets and their monitoring settings

    map< HostAndPort , PingMonitor > PingMonitorThreadManager::targets; 

    // Retrieval commands 

    //TODO
    BSONObj PingMonitorThreadManager::getAllTargetsWithInfo(){
	return BSONObj(); 
    } 

    //TODO
    BSONObj PingMonitorThreadManager::getAllTargets(){
	return BSONObj();
    }

    BSONObj PingMonitorThreadManager::getInfo(){
	return BSONObj();
    }


    //TODO , DON'T call other get functions from within; use iterator once for all
    // possibly call getTargetInfo() from PingMonitor
    BSONObj PingMonitorThreadManager::getTargetInfo( HostAndPort& hp ){
	return BSONObj();
    }

    bool PingMonitorThreadManager::hasTarget( HostAndPort& hp ){
	return ( targets.find( hp ) != targets.end() );
    }

    // includes an "errmsg" field if hp is not a monitoring target
    BSONObj PingMonitorThreadManager::getNetworkType( HostAndPort& hp ){
	BSONObjBuilder toReturn;
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    toReturn.append( "networkType" , i->second.getNetworkType() );
	else
	    toReturn.append( "errmsg" , /*TODO: improve errmsg*/ "target not set" );
	return toReturn.obj();
    }

    // includes an "errmsg" field if hp is not a monitoring target
    BSONObj PingMonitorThreadManager::getCollectionPrefix( HostAndPort& hp ){
    	BSONObjBuilder toReturn;
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    toReturn.append( "collectionPrefix" , i->second.getCollectionPrefix() );
	else
	    toReturn.append( "errmsg" , /*TODO: improve errmsg*/ "target not set" );
	return toReturn.obj();
    }

    // returns false if hp is not being monitored, ALSO if hp is not a monitoring target
    bool PingMonitorThreadManager::isOn( HostAndPort& hp ){
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    return i->second.isOn(); 
	return false;
    } 

    // returns false if hp is not a monitoring target
    bool PingMonitorThreadManager::turnOn( HostAndPort& hp ){
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp ); 
    	if( i != targets.end() ){
	    i->second.turnOn(); 
	    return true;
	}
	return false;
    } 

    // stops monitoring process but does not reset target or clear monitoring history
    // returns false if hp is not a monitoring target
    bool PingMonitorThreadManager::turnOff( HostAndPort& hp ){
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp );
	if( i != targets.end() ){
	    i->second.turnOff(); 
	    return true;
	}
	return false;
    } 

    //TODO
    void PingMonitorThreadManager::clearHistory( HostAndPort& target ){

    }
 
    // if returns false, then hp is not a monitoring target
    bool PingMonitorThreadManager::setInterval( HostAndPort& hp , int nsecs ){ 
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp );
	if( i != targets.end() ){ 
	    i->second.setInterval( nsecs );
	    return true;
	}
	return false; 
    }

    // if returns -1, then hp is not a monitoring target
    int PingMonitorThreadManager::getInterval( HostAndPort& hp ){
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    return i->second.getInterval();
	return -1; 
    }

    // Retrieve from ping monitor storage database
    BSONObj PingMonitorThreadManager::getMonitorResults( HostAndPort& hp ){ 
    	BSONObjBuilder toReturn;
	map< HostAndPort , PingMonitor >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    toReturn.append( "results" , i->second.getMonitorInfo() );
	else
	    toReturn.append( "errmsg" , /*TODO: improve errmsg*/ "target not set" );
	return toReturn.obj();
    }

    // returns errmsg if target is invalid 
    // (if not of master of a network, we can't connect to it, etc)
    BSONObj PingMonitorThreadManager::createTarget( HostAndPort& hp , bool on=true , int interval=15 , string customCollectionPrefix="" ){
	BSONObjBuilder toReturn;
	BSONObj connInfo = canConnect( hp );

	// check if we can connect to the host
	if( connInfo["ok"].boolean() ){
	    
	    BSONObj netInfo = determineNetworkType( hp );

	    // check if host is master of a network, and if so what type of network (cluster, replset)
	    if( netInfo["networkType"].trueValue() ){

		// retrieve custom settings from user if requested, otherwise use defaults
    		if( customCollectionPrefix.empty() )
		    customCollectionPrefix = netInfo["collectionPrefix"].valuestrsafe();
		if( selfSet == false )
		    self = findSelf();

		// store the new PingMonitor object's settings in a BSONObj to return to the user
		BSONObjBuilder newObjData;
		newObjData.append( "on" , on );
		newObjData.append( "interval" , interval );
		newObjData.append( "collectionPrefix" , customCollectionPrefix );

		// actually create the new PingMonitor object
		PingMonitor *pmt = new PingMonitor( hp , self , on , interval , customCollectionPrefix , netInfo["networkType"] );
		pmt->go();
		toReturn.append("ok" , true);
		toReturn.append("newObjData" , newObjData.obj() );
		return toReturn.obj();
	    }
	    else{
		// target is not a network master 
		toReturn.append("ok" , false);
		toReturn.append("errmsg" , "TARGET_NOT_NETWORK_MASTER" );
		//TODO: make an ERRCODES for this class or better yet, throw exception
	    }
	}
	else{
	    // can't make client connection to requested target
	    toReturn.append("ok" , false );
	    toReturn.append("errmsg" , "CAN'T_MAKE_CLIENT_CONNECTION" );
	    //TODO, same as 5 lines up
	    toReturn.append("exceptionMsg" , connInfo["exceptionMsg"] );
	}
	return toReturn.obj();	    
    }

    HostAndPort PingMonitorThreadManager::findSelf(){

	//TODO: actually find self, this is only for testing
    /*
        struct addrinfo *info, *p;
        int gai_result;
        char hostname[1024];
        hostname[1023] = '\0';
        gethostname( hostname, 1023 );

        if( (gai_result = getaddrinfo( hostname, &info ) != 0 ) ){
            fprintf( stderr, "getaddrinfo: %s\n", gai_strerror(gai_result) );
            cout << "failed";
        }

        for( p = info; p!=NULL; p=p->ai_next){
            printf("hostname: %s\n" , p->ai_canonname);
        }
    */
	//selfSet = true;
	HostAndPort hp( "localhost:27017" );
	self = hp;
	return hp;
    }

    BSONObj PingMonitorThreadManager::canConnect( HostAndPort& hp ){
	BSONObjBuilder toReturn;
	try{
	    ScopedDbConnection conn( hp.toString() , socketTimeout );
	    conn.done();
	    toReturn.append( "ok" , true );
	    return toReturn.obj(); 
	}
	catch( UserException& e){
	    toReturn.append( "ok" , false );
	    toReturn.append( "exceptionMsg" , e.toString() );
	    return toReturn.obj();
	}
    }

    // check if the target is the master of a network
    // masters: mongos, primary of replica set
    // not masters: standalone mongod, secondary, arbiter
    BSONObj PingMonitorThreadManager::determineNetworkType( HostAndPort& hp ){
	BSONObjBuilder toReturn;
	BSONObj ismasterResults;
	try{
	    ScopedDbConnection conn( hp.toString() , socketTimeout );
	    conn->runCommand( "admin" , BSON("isMaster"<<1) , ismasterResults );
	    if( ismasterResults["msg"].trueValue() ){
		toReturn.append( "networkType" , "shardedCluster" );
		BSONObj statusResults;
		conn->runCommand( "admin" , BSON("status"<<1) , statusResults );	
		toReturn.append( "collectionPrefix" ,  statusResults.getObjectField("sharding-version")["clusterId"].__oid().toString()); 
	    }
	    if( ismasterResults["setName"].trueValue() ){
		toReturn.append( "networkType" , "replicaSet" );
		toReturn.append( "collectionPrefix" , ismasterResults["setName"].valuestrsafe() );
	    }
	    conn.done();
	}
	catch( DBException& e ){
	    return toReturn.obj();
	}
       return toReturn.obj();
    }

} 
