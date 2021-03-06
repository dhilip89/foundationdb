/*
 * FastTriggeredWatches.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/actorcompiler.h"
#include "fdbrpc/ContinuousSample.h"
#include "fdbclient/NativeAPI.h"
#include "fdbserver/TesterInterface.h"
#include "fdbclient/ReadYourWrites.h"
#include "fdbserver/Knobs.h"
#include "workloads.h"

struct FastTriggeredWatchesWorkload : TestWorkload {
	int nodes, keyBytes;
	double testDuration;
	vector<Future<Void>> clients;
	PerfIntCounter operations, retries;
	Value defaultValue;

	FastTriggeredWatchesWorkload(WorkloadContext const& wcx)
		: TestWorkload(wcx), operations("Operations"), retries("Retries")
	{
		testDuration = getOption( options, LiteralStringRef("testDuration"), 600.0 );
		nodes = getOption( options, LiteralStringRef("nodes"), 100 );
		defaultValue = StringRef(format( "%010d", g_random->randomInt( 0, 1000 ) ));
		keyBytes = std::max( getOption( options, LiteralStringRef("keyBytes"), 16 ), 16 );
	}

	virtual std::string description() { return "Watches"; }

	virtual Future<Void> setup( Database const& cx ) {
		if( clientId == 0 )
			return _setup( cx, this );
		return Void();
	}

	ACTOR Future<Void> _setup( Database cx, FastTriggeredWatchesWorkload* self ) {
		state Transaction tr(cx);

		loop {
			try {
				for(int i = 0; i < self->nodes; i+=2) tr.set(self->keyForIndex(i), self->defaultValue);

				Void _ = wait( tr.commit() );
				break;
			} catch (Error& e) {
				Void _ = wait( tr.onError(e) );
			}
		}

		return Void();
	}

	virtual Future<Void> start( Database const& cx ) {
		if( clientId == 0 )
			return _start( cx, this );
		return Void();
	}

	ACTOR Future<Version> setter( Database cx, Key key, Optional<Value> value ) {
		state ReadYourWritesTransaction tr( cx );
		Void _ = wait( delay( g_random->random01() ) );
		loop {
			try {
				if( value.present() )
					tr.set( key, value.get() );
				else
					tr.clear( key );
				//TraceEvent("FTWSetBegin").detail("key", printable(key)).detail("value", printable(value));
				Void _ = wait( tr.commit() );
				//TraceEvent("FTWSetEnd").detail("key", printable(key)).detail("value", printable(value)).detail("ver", tr.getCommittedVersion());
				return tr.getCommittedVersion();
			} catch( Error &e ) {
				//TraceEvent("FTWSetError").detail("key", printable(key)).detail("value", printable(value)).error(e);
				Void _ = wait( tr.onError(e) );
			}
		}
	}

	ACTOR static Future<Void> _start( Database cx, FastTriggeredWatchesWorkload* self ) {
		state double testStart = now();
		state Version lastReadVersion = 0;
		try {
			loop {
				state double getDuration = 0;
				state double watchEnd = 0;
				state bool first = true;
				state Key setKey = self->keyForIndex(g_random->randomInt(0,self->nodes));
				state Optional<Value> setValue;
				if( g_random->random01() > 0.5 )
					setValue = StringRef(format( "%010d", g_random->randomInt( 0, 1000 )));
				state Future<Version> setFuture = self->setter( cx, setKey, setValue );
				Void _ = wait( delay( g_random->random01() ) );
				loop {
					state ReadYourWritesTransaction tr( cx );

					try {

						Optional<Value> val = wait( tr.get( setKey ) );
						if(!first) {
							getDuration = now() - watchEnd;
						}
						lastReadVersion = tr.getReadVersion().get();
						//TraceEvent("FTWGet").detail("key", printable(setKey)).detail("value", printable(val)).detail("ver", tr.getReadVersion().get());
						if( val == setValue )
							break;
						ASSERT( first );
						state Future<Void> watchFuture = tr.watch( setKey );
						Void _ = wait( tr.commit() );
						//TraceEvent("FTWStartWatch").detail("key", printable(setKey));
						Void _ = wait( watchFuture );
						watchEnd = now();
						first = false;
					} catch( Error &e ) {
						//TraceEvent("FTWWatchError").detail("key", printable(setKey)).error(e);
						Void _ = wait( tr.onError(e) );
					}
				}
				Version ver = wait( setFuture );
				//TraceEvent("FTWWatchDone").detail("key", printable(setKey));
				ASSERT( lastReadVersion - ver >= SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT || lastReadVersion - ver < SERVER_KNOBS->VERSIONS_PER_SECOND*(12+getDuration) );

				if( now() - testStart > self->testDuration )
					break;
			}
			return Void();
		} catch( Error &e ) {
			TraceEvent(SevError, "FastWatchError").error(e,true);
			throw;
		}
	}

	virtual Future<bool> check( Database const& cx ) {
		bool ok = true;
		for( int i = 0; i < clients.size(); i++ )
			if( clients[i].isError() )
				ok = false;
		clients.clear();
		return ok;
	}

	virtual void getMetrics( vector<PerfMetric>& m ) {
		double duration = testDuration;
		m.push_back( PerfMetric( "Operations/sec", operations.getValue() / duration, false ) );
		m.push_back( operations.getMetric() );
		m.push_back( retries.getMetric() );
	}

	Key keyForIndex( uint64_t index ) {
		Key result = makeString( keyBytes );
		uint8_t* data = mutateString( result );
		memset(data, '.', keyBytes);

		double d = double(index) / nodes;
		emplaceIndex( data, 0, *(int64_t*)&d );

		return result;
	}
};

WorkloadFactory<FastTriggeredWatchesWorkload> FastTriggeredWatchesWorkloadFactory("FastTriggeredWatches");