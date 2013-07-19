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

    // map of all targets and their monitoring settings

    map< HostAndPort , PingMonitor* > PingMonitorThreadManager::targets; 

    const string PingMonitorThreadManager::ERRMSG = "errmsg";
    const string PingMonitorThreadManager::NO_SUCH_TARGET = "Server does not exist amongst targets."; 
    const string PingMonitorThreadManager::INVALID_COLLECTION_CHARACTER = "Custom collection prefix name contains invalid character $";
    const string PingMonitorThreadManager::TARGET_NOT_NETWORK_MASTER = "Specified target is not the master of a mongodb network";
    const string PingMonitorThreadManager::ALREADY_USING_COLLECTION = "Custom collection prefix name is already in use by another PingMonitor target";


    // Retrieval commands 

    //TODO
    BSONObj PingMonitorThreadManager::getAllTargetsWithInfo(){
	BSONObjBuilder toReturn;
	for( map< HostAndPort , PingMonitor* >::iterator i = targets.begin(); i!= targets.end(); ++i)
	    toReturn.append( i->first.toString() , i->second->getInfo() );	
	return toReturn.obj();
    } 

    BSONObj PingMonitorThreadManager::getManagerInfo(){
	return BSONObj();
    }


    //TODO , DON'T call other get functions from within; use iterator once for all
    // possibly call getTargetInfo() from PingMonitor
    BSONObj PingMonitorThreadManager::getTargetInfo( HostAndPort& hp ){
    	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    return BSON( "ok" << true << "info" << i->second->getInfo() );
	else
	    return BSON( "ok" << false << ERRMSG << NO_SUCH_TARGET );
    }

    bool PingMonitorThreadManager::hasTarget( HostAndPort& hp ){
	return ( targets.find( hp ) != targets.end() );
    }

    // includes an ERRMSG field if hp is not a monitoring target
    BSONObj PingMonitorThreadManager::getNetworkType( HostAndPort& hp ){
	BSONObjBuilder toReturn;
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    toReturn.append( "networkType" , i->second->getNetworkType() );
	else
	    toReturn.append( ERRMSG , NO_SUCH_TARGET );
	return toReturn.obj();
    }

    // includes an ERRMSG field if hp is not a monitoring target
    BSONObj PingMonitorThreadManager::getCollectionPrefix( HostAndPort& hp ){
    	BSONObjBuilder toReturn;
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    toReturn.append( "collectionPrefix" , i->second->getCollectionPrefix() );
	else
	    toReturn.append( ERRMSG , NO_SUCH_TARGET );
	return toReturn.obj();
    }

    // returns false if hp is not being monitored, ALSO if hp is not a monitoring target
    bool PingMonitorThreadManager::isOn( HostAndPort& hp ){
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    return i->second->isOn(); 
	return false;
    } 

    //TODO
    void PingMonitorThreadManager::clearHistory( HostAndPort& target ){

    }
 
    // if returns -1, then hp is not a monitoring target
    int PingMonitorThreadManager::getInterval( HostAndPort& hp ){
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    return i->second->getInterval();
	return -1; 
    }

    // Retrieve from ping monitor storage database
    BSONObj PingMonitorThreadManager::getMonitorData( HostAndPort& hp ){ 
    	BSONObjBuilder toReturn;
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() )
	    toReturn.append( "results" , i->second->getMonitorInfo() );
	else
	    toReturn.append( ERRMSG , NO_SUCH_TARGET );
	return toReturn.obj();
    }

    bool PingMonitorThreadManager::removeTarget( HostAndPort& hp ){
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() ){
	    // TODO: stop backgroundjob before deleting?
	    // TODO: clear history for this target
	    PingMonitor *pmt = i->second;
	    pmt->shutdown();
	    delete pmt; 
	    targets.erase(i);
	    return true;
	}
	else
	    return false;
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
    		if( customCollectionPrefix.empty() ){
		    customCollectionPrefix = netInfo["collectionPrefix"].valuestrsafe();
		}
		else{
		    // check that the user is not setting two targets to write to the same collection
		    for(map< HostAndPort , PingMonitor* >::iterator i = targets.begin(); i!=targets.end(); i++){
			if( i->second->getCollectionPrefix().compare( customCollectionPrefix ) == 0){
			    toReturn.append( "ok" , false );
			    toReturn.append( ERRMSG , ALREADY_USING_COLLECTION );
			    return toReturn.obj();
			}	
		    } 
		}


		// make sure custom collection prefix does not contain invalid character $
		if( customCollectionPrefix.find("$") == string::npos ){
		    // create the new PingMonitor* object
		    PingMonitor *pmt = new PingMonitor( hp , on , interval , customCollectionPrefix , netInfo["networkType"].valuestrsafe() );
		    pmt->go();
		    targets[ hp ] = pmt;
		    toReturn.append("ok" , true);
		    return toReturn.obj();
		}
		else{
		    // custom collection prefix contains invalid character $ 
		    toReturn.append("ok" , false);
		    toReturn.append(ERRMSG , INVALID_COLLECTION_CHARACTER );
		    return toReturn.obj();
		}

	    }
	    else{
		// target is not a network master 
		toReturn.append("ok" , false);
		toReturn.append(ERRMSG , TARGET_NOT_NETWORK_MASTER );
		//TODO: make an ERRCODES for this class or better yet, throw exception
	    }
	}
	else{
	    // can't make client connection to requested target
	    toReturn.append("ok" , false );
	    toReturn.append(ERRMSG , "CAN'T_MAKE_CLIENT_CONNECTION" );
	    //TODO, same as 5 lines up
	    toReturn.append("exceptionMsg" , connInfo["exceptionMsg"] );
	}
	return toReturn.obj();	    
    }

    bool PingMonitorThreadManager::amendTarget( HostAndPort& hp , bool _on ){
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() ){
	    i->second->setOn( _on );
	    return true;
	}
	else
	    return false;
    }

    bool PingMonitorThreadManager::amendTarget( HostAndPort& hp , int _interval ){
	map< HostAndPort , PingMonitor* >::iterator i = targets.find( hp );
	if( i != targets.end() ){
	    i->second->setInterval( _interval );
	    return true;
	}
	else
	    return false;
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
	BSONObj isMasterResults;

        scoped_ptr<ScopedDbConnection> connPtr;
        try{
            connPtr.reset( new ScopedDbConnection( hp.toString() , socketTimeout ) ); 
            ScopedDbConnection& conn = *connPtr;

	    conn->runCommand( "admin" , BSON("isMaster"<<1) , isMasterResults );
	    if( isMasterResults["msg"].trueValue() ){
		toReturn.append( "networkType" , "shardedCluster" );
		scoped_ptr<DBClientCursor> cursor( conn->query( "config.version" , BSONObj() ) );
		toReturn.append( "collectionPrefix" ,  cursor->next()["clusterId"].__oid().toString()); 

	    }
	    if( isMasterResults["setName"].trueValue() ){
		toReturn.append( "networkType" , "replicaSet" );
		toReturn.append( "collectionPrefix" , isMasterResults["setName"].valuestrsafe() );
	    }
	    
	}
	catch( DBException& e ){
	    return toReturn.obj();
	}
	connPtr->done();
	return toReturn.obj();
    }

} 
