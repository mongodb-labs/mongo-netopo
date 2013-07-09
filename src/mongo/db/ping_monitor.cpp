// ping_monitor.cpp

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

#include "ping_monitor.h"

#include "mongo/base/counter.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/databaseholder.h"
#include "mongo/db/instance.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/util/background.h"


namespace mongo {

    // PingMonitor

    int PingMonitor::numTimes = 0;
    int PingMonitor::getNumTimes(){ return numTimes; }

    void PingMonitor::doPingForHost( const string& hp ){
	numTimes++;
    }

    void PingMonitor::run() {
//	Client::initThread( name().c_str() );
   
	while ( ! inShutdown() ) {

	    sleepsecs( 2 );
	    LOG(3) << "PingMonitor thread awake" << endl;

	    if( false /*lockedForWriting()*/ ) {
		// note: this is not perfect as you can go into fsync+lock between
		// this and actually doing the delete later
		LOG(3) << " locked for writing" << endl;
		continue;
	    }

	    // if part of a replSet but not in a readable state ( e.g. during initial sync), skip
	    //if ( theReplSet && !theReplSet->state().readable() )
	    //    continue;

	    //TODO: change this to defaulting to self; also to being settable
	    doPingForHost( "localhost:30999" );
	}
    }

    void startPingBackgroundJob() {
	PingMonitor* pmt = new PingMonitor();
	pmt->go();
    }

} 
