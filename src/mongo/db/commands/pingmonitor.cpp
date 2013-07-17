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

#include <time.h>
#include "boost/date_time/posix_time/posix_time.hpp"
#include "mongo/client/parallel.h"
#include "mongo/client/connpool.h"
#include <sstream>
#include "mongo/util/assert_util.h"
#include "mongo/db/ping_monitor.h"

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

    class TurnOnPingMonitorCommand : public Command {
    public:
	TurnOnPingMonitorCommand() : Command( "turnOnPingMonitor" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Starts the background PingMonitor process if the target is set." ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

	    if( PingMonitor::getTargetIsSet() ){
		result.append( "target" , PingMonitor::getTarget());
		if( PingMonitor::turnOnMonitoring() )
		    return true;
		else
		    return false; 
	    }
	    else{
		result.append("errmsg" , "Target is not set. Use database command { 'setPingMonitorTarget' : '<host:port>' } to set target, then turn on monitoring.");
	    }
	    return true;
	}
    } turnOnPingMonitorCmd;

    class TurnOffPingMonitorCommand : public Command {
    public:
	TurnOffPingMonitorCommand() : Command( "turnOffPingMonitor" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Stops the background PingMonitor process. Does not reset the target or clear monitoring history." ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
	    if( PingMonitor::turnOffMonitoring() )
		return true;
	    else
		return false;
	}
    } turnOffPingMonitorCmd;

    class SetPingMonitorTargetCommand : public Command {
    public:
	SetPingMonitorTargetCommand() : Command( "setPingMonitorTarget" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Sets the target to the input host:port." ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
	    string empty = "";
	    if( empty.compare( cmdObj["setPingMonitorTarget"].valuestrsafe() ) == 0 ) {
		result.append("errmsg" , "Please give a valid host:port string for the target, ie { 'setPingMonitorTarget' : 'localhost:27017' }. ");
		return false;
	    }
	    try{ 
		HostAndPort p( cmdObj["setPingMonitorTarget"].valuestrsafe() );
		BSONObj setInfo = PingMonitor::setTarget( p );
		if( setInfo["ok"].boolean() ){
		    result.append( "newTarget" , PingMonitor::getTarget().toString() );
		    result.append( "isMonitoring" , PingMonitor::getIsMonitoring() );
		    return true;
		}
		else{
		    result.append("errmsg" , setInfo["errmsg"].valuestrsafe() );
		    return false;
		}
	    } catch( std::exception& ex){
		cout << "std::exception: " << ex.what() << endl;
	    }

	    return false;
	}
    } setPingMonitorCmd;

    class CheckPingMonitorTargetCommand : public Command {
    public:
	CheckPingMonitorTargetCommand() : Command( "checkPingMonitorTarget" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Returns the host:port that is currently set as the target, or reports that the target is not set." ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
	    if( PingMonitor::getTargetIsSet() )
		result.append("target" , PingMonitor::getTarget() );
	    else
		result.append("msg" , "Target is not set. Set using db command { 'setPingMonitorTarget' : '<host:port>' }. ");
	    return true;
	}
    } checkPingMonitorTargetCmd;

    class SetPingMonitorIntervalCommand : public Command {
    public:
	SetPingMonitorIntervalCommand() : Command( "setPingMonitorInterval" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Sets the PingMonitor background process to run every n seconds." ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
	    if( PingMonitor::setPingInterval( cmdObj["setPingMonitorInterval"].numberInt() ) )
		return true;
	    else
		return false;
	}
    } setPingMonitorIntervalCmd;

    class CheckPingMonitorIntervalCommand : public Command {
    public:
	CheckPingMonitorIntervalCommand() : Command( "checkPingMonitorInterval" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Reports how often (in seconds) the PingMonitor background process runs." ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
	    result.append("numSecs" , PingMonitor::getPingInterval() );
	    return true;
	}
    } checkPingMonitorIntervalCmd;

    class PingMonitorConfCommand : public Command {
    public:
	PingMonitorConfCommand() : Command( "pingMonitorConfigure" ) {}
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

	    vector<BSONElement> targets = cmdObj.getField("targets").Array();

	    for( vector<BSONElement>::iterator i=targets.begin(); i!=targets.end(); ++i){
		BSONElement be = *it;
		BSONObject bo = be.embeddedObject();
		cout << bo.toString() << endl;		
	    }

	    return true;
	}
    } pingMonitorConfCmd;

class ShowPingMonitorResultsCommand : public Command {
    public:
	ShowPingMonitorResultsCommand() : Command( "showPingMonitorResults" ) {}
	virtual bool slaveOk() const { return true; } //might want to make this false later
	virtual void help( stringstream &help ) const { help << "Displays results from the background PingMonitor process" ; }
	virtual LockType locktype() const { return NONE; }
	virtual void addRequiredPrivileges( const std::string& dbname,
					    const BSONObj& cmdObj,
					    std::vector<Privilege>* out) {} // No auth required

	virtual bool run( const string& badns, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {

	    if( ( cmdObj["target"].trueValue() ) ) {
		string targetString = cmdObj.getStringField("target");
		try{
		    HostAndPort newTarget( targetString );
	        if( PingMonitor::setTarget( newTarget )["ok"].boolean() ){
			result.append("newTarget" , true ); 
			PingMonitor::turnOnMonitoring();
		    }
		    else{
			result.append("errmsg" , "Input target is not valid");
			return false;
		    }
		} catch( DBException& e ){}
	    }


	    if( PingMonitor::getTargetIsSet() == false ){
		result.append( "errmsg" , "Target is not set. Set the target and immediately begin monitoring by including document { 'target' : '<host:port>' } in the pingMonitor command, or set the target but defer monitoring by using the db command { 'setPingMonitorTarget' : '<host:port>' } ." );
		return false;
	    }
	    else if( PingMonitor::getIsMonitoring() == false ){
		result.append( "errmsg" , "Ping monitoring is not turned on. Turn on using db command { 'turnOnPingMonitoring' : 1 }. In order to turn on monitoring, the monitoring target must be set. Check the target using db command { 'checkPingMonitorTarget' : 1 }. Set or reset the target using db command { 'setPingMonitorTarget' : '<host:port>' }. "  );
		return false;
	    } 
	    else{
		result.append("target" , PingMonitor::getTarget().toString(true) ); 
		result.append("output" , PingMonitor::getMonitorResults( ));
	    }
	    return true;
	}

    } showPingMonitorResultsCmd;
       return true;
    }

}
