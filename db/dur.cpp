// @file dur.cpp durability in the storage engine (crash-safeness / journaling)

/**
*    Copyright (C) 2009 10gen Inc.
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

/*
   phases

     PREPLOGBUFFER
       we will build an output buffer ourself and then use O_DIRECT
       we could be in read lock for this
       for very large objects write directly to redo log in situ?
     WRITETOJOURNAL
       we could be unlocked (the main db lock that is...) for this, with sufficient care, but there is some complexity
         have to handle falling behind which would use too much ram (going back into a read lock would suffice to stop that).
         for now we are in read lock which is not ideal.
     WRITETODATAFILES
       apply the writes back to the non-private MMF after they are for certain in redo log
     REMAPPRIVATEVIEW
       we could in a write lock quickly flip readers back to the main view, then stay in read lock and do our real
         remapping. with many files (e.g., 1000), remapping could be time consuming (several ms), so we don't want
         to be too frequent.  tracking time for this step would be wise.
       there could be a slow down immediately after remapping as fresh copy-on-writes for commonly written pages will
         be required.  so doing these remaps more incrementally in the future might make sense - but have to be careful
         not to introduce bugs.

     @see https://docs.google.com/drawings/edit?id=1TklsmZzm7ohIZkwgeK6rMvsdaR13KjtJYMsfLr175Zc
*/

#include "pch.h"
#include "cmdline.h"
#include "client.h"
#include "dur.h"
#include "dur_journal.h"
#include "dur_commitjob.h"
#include "dur_recover.h"
#include "../util/concurrency/race.h"
#include "../util/mongoutils/hash.h"
#include "../util/mongoutils/str.h"
#include "../util/timer.h"
#include "dur_stats.h"

using namespace mongoutils;

namespace mongo {

    namespace dur {

        void WRITETODATAFILES();
        void PREPLOGBUFFER();

        /** declared later in this file
            only used in this file -- use DurableInterface::commitNow() outside
        */
        static void groupCommit();

        CommitJob commitJob;

        Stats stats;

        void Stats::S::reset() {
            memset(this, 0, sizeof(*this));
        }

        Stats::Stats() {
            _a.reset();
            _b.reset();
            curr = &_a;
            _intervalMicros = 3000000;
        }

        Stats::S * Stats::other() {
            return curr == &_a ? &_b : &_a;
        }

        BSONObj Stats::S::_asObj() {
            return BSON(
                       "commits" << _commits <<
                       "journaledMB" << _journaledBytes / 1000000.0 <<
                       "writeToDataFilesMB" << _writeToDataFilesBytes / 1000000.0 <<
                       "commitsInWriteLock" << _commitsInWriteLock <<
                       "timeMs" <<
                       BSON( "dt" << _dtMillis <<
                             "prepLogBuffer" << (unsigned) (_prepLogBufferMicros/1000) <<
                             "writeToJournal" << (unsigned) (_writeToJournalMicros/1000) <<
                             "writeToDataFiles" << (unsigned) (_writeToDataFilesMicros/1000) <<
                             "remapPrivateView" << (unsigned) (_remapPrivateViewMicros/1000)
                           )
                   );
        }

        BSONObj Stats::asObj() {
            return other()->_asObj();
        }

        void Stats::rotate() {
            unsigned long long now = curTimeMicros64();
            unsigned long long dt = now - _lastRotate;
            if( dt >= _intervalMicros && _intervalMicros ) {
                // rotate
                curr->_dtMillis = (unsigned) (dt/1000);
                _lastRotate = now;
                curr = other();
            }
        }

        void NonDurableImpl::setNoJournal(void *dst, void *src, unsigned len) {
            memcpy(dst, src, len);
        }

        void DurableImpl::setNoJournal(void *dst, void *src, unsigned len) {
            // for now, journalled
            memcpy( writingPtr(dst, len), src, len );

            /* todo before doing this:
               - finish implementation of _switchToReachableView
               - performance test it.  privateViews.find() uses a mutex, so that could make
                 it slow.
            */
            /*
            if( testIntent ) {
                memcpy(MongoMMF::switchToPrivateView(dst), src, len);
                return;
            }

            void *writeView = MongoMMF::_switchToWritableView(dst);
            memcpy(writeView, src, len);
            if( memcmp(writeView, src, len) ) {
                // a copy of the page exists, so need to write it there also
                memcpy(dst, src, len);
            }
            */
        }

        /** base declare write intent function that all the helpers call. */
        void DurableImpl::declareWriteIntent(void *p, unsigned len) {
            commitJob.note(p, len);
        }

        static DurableImpl* durableImpl = new DurableImpl();
        static NonDurableImpl* nonDurableImpl = new NonDurableImpl();
        DurableInterface* DurableInterface::_impl = nonDurableImpl;

        void DurableInterface::enableDurability() {
            assert(_impl == nonDurableImpl);
            _impl = durableImpl;
        }

        void DurableInterface::disableDurability() {
            assert(_impl == durableImpl);
            massert(13616, "can't disable durability with pending writes", !commitJob.hasWritten());
            _impl = nonDurableImpl;
        }

        bool DurableImpl::commitNow() {
            groupCommit();
            return true;
        }

        bool DurableImpl::awaitCommit() {
            commitJob.awaitNextCommit();
            return true;
        }

        /** Declare that a file has been created
            Normally writes are applied only after journalling, for safety.  But here the file
            is created first, and the journal will just replay the creation if the create didn't
            happen because of crashing.
        */
        void DurableImpl::createdFile(string filename, unsigned long long len) {
            shared_ptr<DurOp> op( new FileCreatedOp(filename, len) );
            commitJob.noteOp(op);
        }

        /** indicate that a database is about to be dropped.  call before the actual drop. */
        void DurableImpl::droppingDb(string db) {
            shared_ptr<DurOp> op( new DropDbOp(db) );

            // DropDbOp must be in a commit group by itself to ensure proper
            // sequencing because all DurOps are applied before any WriteOps.
            groupCommit();
            commitJob.noteOp(op);
            groupCommit();
        }

        void* DurableImpl::writingPtr(void *x, unsigned len) {
            void *p = x;
            declareWriteIntent(p, len);
            return p;
        }

        /** declare intent to write
            @param ofs offset within buf at which we will write
            @param len the length at ofs we will write
            @return new buffer pointer.
        */
        void* DurableImpl::writingAtOffset(void *buf, unsigned ofs, unsigned len) {
            char *p = (char *) buf;
            declareWriteIntent(p+ofs, len);
            return p;
        }

        void* DurableImpl::writingRangesAtOffsets(void *buf, const vector< pair< long long, unsigned > > &ranges ) {
            char *p = (char *) buf;
            for( vector< pair< long long, unsigned > >::const_iterator i = ranges.begin();
                    i != ranges.end(); ++i ) {
                declareWriteIntent( p + i->first, i->second );
            }
            return p;
        }

        void DurableImpl::commitIfNeeded() {
            if (commitJob.bytes() > 50*1024*1024) // should this also fire if CmdLine::DurAlwaysCommit?
                groupCommit();
        }

        /** Used in _DEBUG builds to check that we didn't overwrite the last intent
            that was declared.  called just before writelock release.  we check a few
            bytes after the declared region to see if they changed.

            @see MongoMutex::_releasedWriteLock

            SLOW
        */
#if 0
        void DurableImpl::debugCheckLastDeclaredWrite() {
            static int n;
            ++n;

            assert(debug && cmdLine.dur);
            if (commitJob.writes().empty())
                return;
            const WriteIntent &i = commitJob.lastWrite();
            size_t ofs;
            MongoMMF *mmf = privateViews.find(i.start(), ofs);
            if( mmf == 0 )
                return;
            size_t past = ofs + i.length();
            if( mmf->length() < past + 8 )
                return; // too close to end of view
            char *priv = (char *) mmf->getView();
            char *writ = (char *) mmf->view_write();
            unsigned long long *a = (unsigned long long *) (priv+past);
            unsigned long long *b = (unsigned long long *) (writ+past);
            if( *a != *b ) {
                for( set<WriteIntent>::iterator it(commitJob.writes().begin()), end((commitJob.writes().begin())); it != end; ++it ) {
                    const WriteIntent& wi = *it;
                    char *r1 = (char*) wi.start();
                    char *r2 = (char*) wi.end();
                    if( r1 <= (((char*)a)+8) && r2 > (char*)a ) {
                        //log() << "it's ok " << wi.p << ' ' << wi.len << endl;
                        return;
                    }
                }
                log() << "dur data after write area " << i.start() << " does not agree" << endl;
                log() << " was:  " << ((void*)b) << "  " << hexdump((char*)b, 8) << endl;
                log() << " now:  " << ((void*)a) << "  " << hexdump((char*)a, 8) << endl;
                log() << " n:    " << n << endl;
                log() << endl;
            }
        }
#endif

        /** write the buffer we have built to the journal and fsync it.
            outside of lock as that could be slow.
        */
        static void WRITETOJOURNAL(AlignedBuilder& ab) {
            Timer t;
            journal(ab);
            stats.curr->_writeToJournalMicros += t.micros();
        }

        // Functor to be called over all MongoFiles

        class validateSingleMapMatches {
        public:
            validateSingleMapMatches(unsigned long long& bytes) :_bytes(bytes)  {}
            void operator () (MongoFile *mf) {
                if( mf->isMongoMMF() ) {
                    MongoMMF *mmf = (MongoMMF*) mf;
                    const char *p = (const char *) mmf->getView();
                    const char *w = (const char *) mmf->view_write();

                    if (!p && !w) return;

                    assert(p);
                    assert(w);

                    _bytes += mmf->length();

                    assert( mmf->length() == (unsigned) mmf->length() );
                    if (memcmp(p, w, (unsigned) mmf->length()) == 0)
                        return; // next file

                    unsigned low = 0xffffffff;
                    unsigned high = 0;
                    log() << "DurParanoid mismatch in " << mmf->filename() << endl;
                    int logged = 0;
                    unsigned lastMismatch = 0xffffffff;
                    for( unsigned i = 0; i < mmf->length(); i++ ) {
                        if( p[i] != w[i] ) {
                            if( lastMismatch != 0xffffffff && lastMismatch+1 != i )
                                log() << endl; // separate blocks of mismatches
                            lastMismatch= i;
                            if( ++logged < 60 ) {
                                stringstream ss;
                                ss << "mismatch ofs:" << hex << i <<  "\tfilemap:" << setw(2) << (unsigned) w[i] << "\tprivmap:" << setw(2) << (unsigned) p[i];
                                if( p[i] > 32 && p[i] <= 126 )
                                    ss << '\t' << p[i];
                                log() << ss.str() << endl;
                            }
                            if( logged == 60 )
                                log() << "..." << endl;
                            if( i < low ) low = i;
                            if( i > high ) high = i;
                        }
                    }
                    if( low != 0xffffffff ) {
                        std::stringstream ss;
                        ss << "dur error warning views mismatch " << mmf->filename() << ' ' << (hex) << low << ".." << high << " len:" << high-low+1;
                        log() << ss.str() << endl;
                        log() << "priv loc: " << (void*)(p+low) << ' ' << endl;
                        set<WriteIntent>& b = commitJob.writes();
                        (void)b; // mark as unused. Useful for inspection in debugger

                        massert(13599, "Written data does not match in-memory view. Missing WriteIntent?", false);
                    }
                }
            }
        private:
            unsigned long long& _bytes;
        };

        /** (SLOW) diagnostic to check that the private view and the non-private view are in sync.
        */
        void debugValidateAllMapsMatch() {
            if( ! (cmdLine.durOptions & CmdLine::DurParanoid) )
                return;

            unsigned long long bytes = 0;
            Timer t;
            MongoFile::forEach(validateSingleMapMatches(bytes));
            OCCASIONALLY log() << "DurParanoid map check " << t.millis() << "ms for " <<  (bytes / (1024*1024)) << "MB" << endl;
        }

        /** We need to remap the private views periodically. otherwise they would become very large.
            Call within write lock.
        */
        void _REMAPPRIVATEVIEW() {
            static unsigned startAt;
            static unsigned long long lastRemap;

            dbMutex.assertWriteLocked();
            dbMutex._remapPrivateViewRequested = false;
            assert( !commitJob.hasWritten() );

            if( 0 ) {
                log() << "TEMP remapprivateview disabled for testing - will eventually run oom in this mode if db bigger than ram" << endl;
                return;
            }

            // we want to remap all private views about every 2 seconds.  there could be ~1000 views so
            // we do a little each pass; beyond the remap time, more significantly, there will be copy on write
            // faults after remapping, so doing a little bit at a time will avoid big load spikes on
            // remapping.
            unsigned long long now = curTimeMicros64();
            double fraction = (now-lastRemap)/20000000.0;

            rwlock lk(MongoFile::mmmutex, false);
            set<MongoFile*>& files = MongoFile::getAllFiles();
            unsigned sz = files.size();
            if( sz == 0 )
                return;

            unsigned ntodo = (unsigned) (sz * fraction);
            if( ntodo < 1 ) ntodo = 1;
            if( ntodo > sz ) ntodo = sz;

            const set<MongoFile*>::iterator b = files.begin();
            const set<MongoFile*>::iterator e = files.end();
            set<MongoFile*>::iterator i = b;
            // skip to our starting position
            for( unsigned x = 0; x < startAt; x++ ) {
                i++;
                if( i == e ) i = b;
            }
            startAt = (startAt + ntodo) % sz; // mark where to start next time

            for( unsigned x = 0; x < ntodo; x++ ) {
                dassert( i != e );
                if( (*i)->isMongoMMF() ) {
                    MongoMMF *mmf = (MongoMMF*) *i;
                    assert(mmf);
                    if( mmf->willNeedRemap() ) {
                        mmf->willNeedRemap() = false;
                        mmf->remapThePrivateView();
                    }
                    i++;
                    if( i == e ) i = b;
                }
            }
        }
        void REMAPPRIVATEVIEW() {
            Timer t;
            _REMAPPRIVATEVIEW();
            stats.curr->_remapPrivateViewMicros += t.micros();
        }

        void drainSome() {
            Writes& writes = commitJob.wi();
            writes._deferred.invoke();
        }

        static void _groupCommit() {
            stats.curr->_commits++;

            if( !commitJob.hasWritten() ) {
                // getlasterror request could have came after the data was already committed
                commitJob.notifyCommitted();
                return;
            }

            PREPLOGBUFFER();

            WRITETOJOURNAL(commitJob._ab);

            // data is now in the journal, which is sufficient for acknowledging getLastError.
            // (ok to crash after that)
            commitJob.notifyCommitted();

            WRITETODATAFILES();

            commitJob.reset();

            // REMAPPRIVATEVIEW
            //
            // remapping private views must occur after WRITETODATAFILES otherwise
            // we wouldn't see newly written data on reads.
            //
            DEV assert( !commitJob.hasWritten() );
            if( !dbMutex.isWriteLocked() ) {
                // this needs done in a write lock thus we do it on the next acquisition of that
                // instead of here (there is no rush if you aren't writing anyway -- but it must happen,
                // if it is done, before any uncommitted writes occur).
                //
                dbMutex._remapPrivateViewRequested = true;
            }
            else {
                stats.curr->_commitsInWriteLock++;
                // however, if we are already write locked, we must do it now -- up the call tree someone
                // may do a write without a new lock acquisition.  this can happen when MongoMMF::close() calls
                // this method when a file (and its views) is about to go away.
                //
                REMAPPRIVATEVIEW();
            }
        }
        /** locking in read lock when called
            @see MongoMMF::close()
        */
        static void groupCommit() {
            dbMutex.assertAtLeastReadLocked();
            try {
                _groupCommit();
            }
            catch(std::exception& e) {
                log() << "exception in dur::groupCommit causing immediate shutdown: " << e.what() << endl;
                abort(); // based on myTerminate()
            }
        }

        static void go() {
            if( !commitJob.hasWritten() )
                return;

            {
                readlocktry lk("", 1000);
                if( lk.got() ) {
                    groupCommit();
                    return;
                }
            }

            // starvation on read locks could occur.  so if read lock acquisition is slow, try to get a
            // write lock instead.  otherwise writes could use too much RAM.
            writelock lk;
            groupCommit();
        }

        /** called when a MongoMMF is closing -- we need to go ahead and group commit in that case before its
            views disappear
        */
        void closingFileNotification() {
            if (!cmdLine.dur)
                return;

            if( dbMutex.atLeastReadLocked() ) {
                groupCommit();
            }
            else {
                assert( inShutdown() );
                if( commitJob.hasWritten() ) {
                    log() << "dur warning files are closing outside locks with writes pending" << endl;
                }
            }
        }

        mongo::mutex durThreadMutex("durthreadmtx");

        CodeBlock durThreadMain;

        void durThread() {
            Client::initThread("dur");
            const int HowOftenToGroupCommitMs = 90;
            while( 1) {
                sleepmillis(10);
                scoped_lock lk(durThreadMutex);
                CodeBlock::Within w(durThreadMain);
                try {
                    int millis = HowOftenToGroupCommitMs;
                    {
                        Timer t;
                        journalRotate(); // note we do this part outside of mongomutex
                        millis -= t.millis();
                        assert( millis <= HowOftenToGroupCommitMs );
                        if( millis < 5 )
                            millis = 5;
                    }

                    // we do this in a couple blocks, which makes it a tiny bit faster (only a little) on throughput,
                    // but is likely also less spiky on our cpu usage, which is good:
                    sleepmillis(millis/2);
                    drainSome();
                    sleepmillis(millis/2);
                    drainSome();

                    go();
                    stats.rotate();
                }
                catch(std::exception& e) {
                    log() << "exception in durThread causing immediate shutdown: " << e.what() << endl;
                    abort(); // based on myTerminate()
                }
            }
        }

        void recover();

        void releasingWriteLock() {
            try {
                if (commitJob.bytes() > 100*1024*1024 || cmdLine.durOptions & CmdLine::DurAlwaysCommit)
                    groupCommit();
            }
            catch(std::exception& e) {
                log() << "exception in dur::releasingWriteLock causing immediate shutdown: " << e.what() << endl;
                abort(); // based on myTerminate()
            }
        }

        /** at startup, recover, and then start the journal threads */
        void startup() {
            if( !cmdLine.dur )
                return;

            DurableInterface::enableDurability();

            journalMakeDir();
            try {
                recover();
            }
            catch(...) {
                log() << "exception during recovery" << endl;
                throw;
            }

            boost::thread t(durThread);
        }

        TempDisableDurability::TempDisableDurability() : _wasDur(cmdLine.dur), _lock(durThreadMutex) {
            dbMutex.assertWriteLocked();
            if (_wasDur) {
                DurableInterface::disableDurability();
                cmdLine.dur = false;
            }
        }

        TempDisableDurability::~TempDisableDurability() {
            dbMutex.assertWriteLocked();
            if (_wasDur) {
                cmdLine.dur = true;
                DurableInterface::enableDurability();
            }
        }

    } // namespace dur

} // namespace mongo
