/** @file pingmonitor.cpp commands suited for any mongo server (both mongod, mongos) */

/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/client/parallel.h"
#include "mongo/client/connpool.h"
#include <sstream>
#include "mongo/util/assert_util.h"
#include "mongo/db/ping_monitor_thread_manager.h"

#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_rs.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/background.h"
#include "mongo/db/commands.h"
#include "mongo/db/db.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/repl/multicmd.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/server.h"
#include "mongo/db/stats/counters.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/lruishmap.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/processinfo.h"
#include "mongo/util/ramlog.h"
#include "mongo/util/version.h"

namespace mongo {

    // PingMonitor commands

    class PingMonitorConfigureCommand : public Command {
    public:
	PingMonitorConfigureCommand() : Command( "pingMonitorConfigure" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Allows adding target networks to run ping monitoring on, and configuring the ping monitor for each network." ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
	    
	    if( cmdObj["targets"].trueValue() == false ){
		result.append( "errmsg" , "No targets specified." );
		return false;
	    }

	    if( cmdObj.getObjectField("targets").couldBeArray() ){
		vector<BSONElement> targets = cmdObj.getField("targets").Array();

		for( vector<BSONElement>::iterator i=targets.begin(); i!=targets.end(); ++i){
		    BSONElement be = *i;
		    BSONObj bo = be.embeddedObject();
		    HostAndPort hp;
		    try{
			hp = HostAndPort( bo["server"].valuestrsafe() );
		    }
		    catch( DBException& e ){
			result.append( bo["server"].valuestrsafe() , "Not a valid host:port pair." );
			continue;
		    }
		    // delete exisitng target 
		    if( bo["remove"].trueValue() ){
			if( PingMonitorThreadManager::hasTarget( hp ) ){
			    PingMonitorThreadManager::removeTarget( hp );
			    result.append( bo["server"].valuestrsafe() , "Fully deleted from system" );
			}
			else
			    result.append( bo["server"].valuestrsafe() , "Server does not exist amongst monitoring targets" );
		    }
		    // update target settings
		    else if( PingMonitorThreadManager::hasTarget( hp ) ){
			if( bo["collectionPrefix"].trueValue() ){
			    result.append( bo["server"].valuestrsafe() , "Cannot reset collectionPrefix. To use this prefix, ensure no other target is using it and then remove this target using db.runCommand( { pingMonitorConfigure : 1 , targets : [ { server : <host:port> , remove : true } ] } ) , then re-create the target with the desired collectionPrefix. Warning: by doing this, all monitoring data up to this point will be lost." );	    
			    continue;
			}
			if( bo.getField("on").eoo() == false ){
			    PingMonitorThreadManager::amendTarget( hp , bo.getBoolField("on") );
			}
			if( bo["interval"].trueValue() && bo["interval"].isNumber() )
			    PingMonitorThreadManager::amendTarget( hp , bo["interval"].numberInt() );		    
			result.append( bo["server"].valuestrsafe() , PingMonitorThreadManager::getTargetInfo( hp )["info"].embeddedObject() );
		    }
		    // create new target
		    else{
			bool on = true;
			int interval = 15;
			string collectionPrefix = "";	
			if( bo.getField("on").eoo() == false )
			    on = bo.getBoolField("on");
			if( bo["interval"].trueValue() && bo["interval"].isNumber() )
			    interval = bo["interval"].numberInt();
			if( bo["collectionPrefix"].trueValue() )
			    collectionPrefix = bo["collectionPrefix"].valuestrsafe(); 
			BSONObj success = PingMonitorThreadManager::createTarget( hp , on , interval , collectionPrefix );
			if( success["ok"].boolean() ){
			    result.append( bo["server"].valuestrsafe() , PingMonitorThreadManager::getTargetInfo( hp )["info"].embeddedObject() );
			    continue;
			}
			else
			    result.append( bo["server"].valuestrsafe() , success["errmsg"].valuestrsafe() );
		    }
		}
	    }
	    else{
		    result.append("errmsg" , "Invalid input format for targets. Please use an array of JSON documents, for example, db.runCommand( { pingMonitorConfigure : 1 , targets : [ { server : <host:port> , on : true } )." ); 
	    }
	    return true;
	}
    }pingMonitorConfigureCmd;

    class PingMonitorSettingsCommand : public Command {
	public:
	    PingMonitorSettingsCommand() : Command( "pingMonitorSettings" ) {}
	    virtual bool slaveOk() const { return true; } //might want to make this false later
	    virtual void help( stringstream &help ) const { help << "Display the list of current targets and their monitoring settings" ; }
	    virtual LockType locktype() const { return NONE; }
	    virtual void addRequiredPrivileges( const std::string& dbname,
						const BSONObj& cmdObj,
						std::vector<Privilege>* out) {} // No auth required

	    virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

		// if no specific targets specified, show information about all targets 
		if( cmdObj["targets"].trueValue() == false ){
		    result.append("targets" , PingMonitorThreadManager::getAllTargetsWithInfo() );
		    return true;
		}

		// else, show informaiton only about specified targets
		vector<BSONElement> targets = cmdObj.getField("targets").Array();
		for( vector<BSONElement>::iterator i=targets.begin(); i!=targets.end(); ++i){
		    BSONElement be = *i;
		    BSONObj bo = be.embeddedObject();
		    HostAndPort hp;
		    try{
			hp = HostAndPort( bo["server"].valuestrsafe() );
		    }
		    catch( DBException& e ){
			result.append( bo["server"].valuestrsafe() , "Not a valid host:port pair." );
			continue;
		    }
		    result.append( bo["server"].valuestrsafe() , PingMonitorThreadManager::getTargetInfo( hp )["info"].embeddedObject() );
		}
		return true;
	    }

	} PingMonitorSettingsCmd;

    class PingMonitorDataCommand : public Command {
	public:
	    PingMonitorDataCommand() : Command( "pingMonitorData" ) {}
	    virtual bool slaveOk() const { return true; } //might want to make this false later
	    virtual void help( stringstream &help ) const { help << "Displays results from the background PingMonitor process" ; }
	    virtual LockType locktype() const { return NONE; }
	    virtual void addRequiredPrivileges( const std::string& dbname,
						const BSONObj& cmdObj,
						std::vector<Privilege>* out) {} // No auth required

	    virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
		if( cmdObj["target"].trueValue() ){
		    HostAndPort hp;
		    try{
			hp = HostAndPort( cmdObj["target"].valuestrsafe() );
		    }
		    catch( DBException& e ){
			result.append( cmdObj["target"].valuestrsafe() , "Not a valid host:port pair." );
			return false;
		    } 
		    result.append( cmdObj["target"].valuestrsafe() , PingMonitorThreadManager::getMonitorData( hp ) );
		}
		return true;
	    }

	} PingMonitorDataCmd;





}
