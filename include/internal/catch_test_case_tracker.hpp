/*
 *  Created by Phil Nash on 23/7/2013
 *  Copyright 2013 Two Blue Cubes Ltd. All rights reserved.
 *
 *  Distributed under the Boost Software License, Version 1.0. (See accompanying
 *  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
 */
#ifndef TWOBLUECUBES_CATCH_TEST_CASE_TRACKER_HPP_INCLUDED
#define TWOBLUECUBES_CATCH_TEST_CASE_TRACKER_HPP_INCLUDED

#include "catch_compiler_capabilities.h"
#include "catch_ptr.hpp"

#include <map>
#include <string>
#include <assert.h>
#include <vector>

namespace Catch {
namespace TestCaseTracking {

    struct ITracker : SharedImpl<> {
        virtual ~ITracker();
        
        // static queries
        virtual std::string name() const = 0;
        
        // dynamic queries
        virtual bool isComplete() const = 0; // Successfully completed or failed
        virtual bool isSuccessfullyCompleted() const = 0;
        virtual bool isOpen() const = 0; // Started but not complete
        
        virtual ITracker& parent() = 0;
        
        // actions
        virtual void close() = 0; // Successfully complete
        virtual void fail() = 0;
        virtual void markAsNeedingAnotherRun() = 0;
        
        virtual void addChild( Ptr<ITracker> const& child ) = 0;
        virtual ITracker* findChild( std::string const& name ) = 0;
        virtual void openChild() = 0;
    };
    
    class TrackerContext {
        
        enum RunState {
            NotStarted,
            Executing,
            CompletedCycle
        };
        
        Ptr<ITracker> m_rootTracker;
        ITracker* m_currentTracker;
        RunState m_runState;
        
    public:
        
        static TrackerContext& instance() {
            static TrackerContext s_instance;
            return s_instance;
        }
        
        TrackerContext()
        :   m_currentTracker( CATCH_NULL ),
            m_runState( NotStarted )
        {}
        
        
        ITracker& startRun();
        
        void endRun() {
            m_rootTracker.reset();
            m_currentTracker = CATCH_NULL;
            m_runState = NotStarted;
        }
        
        void startCycle() {
            m_currentTracker = m_rootTracker.get();
            m_runState = Executing;
        }
        void completeCycle() {
            m_runState = CompletedCycle;
        }
        
        bool completedCycle() const {
            return m_runState == CompletedCycle;
        }
        ITracker& currentTracker() {
            return *m_currentTracker;
        }
        void setCurrentTracker( ITracker* tracker ) {
            m_currentTracker = tracker;
        }
    };
    
    class TrackerBase : public ITracker {
    protected:
        enum CycleState {
            NotStarted,
            Executing,
            ExecutingChildren,
            NeedsAnotherRun,
            CompletedSuccessfully,
            Failed
        };
        class TrackerHasName {
            std::string m_name;
        public:
            TrackerHasName( std::string const& name ) : m_name( name ) {}
            bool operator ()( Ptr<ITracker> const& tracker ) {
                return tracker->name() == m_name;
            }
        };
        typedef std::vector<Ptr<ITracker> > Children;
        std::string m_name;
        TrackerContext& m_ctx;
        ITracker* m_parent;
        Children m_children;
        CycleState m_runState;
    public:
        TrackerBase( std::string const& name, TrackerContext& ctx, ITracker* parent )
        :   m_name( name ),
            m_ctx( ctx ),
            m_parent( parent ),
            m_runState( NotStarted )
        {}
        virtual ~TrackerBase();
        
        virtual std::string name() const CATCH_OVERRIDE {
            return m_name;
        }
        virtual bool isComplete() const CATCH_OVERRIDE {
            return m_runState == CompletedSuccessfully || m_runState == Failed;
        }
        virtual bool isSuccessfullyCompleted() const CATCH_OVERRIDE {
            return m_runState == CompletedSuccessfully;
        }
        virtual bool isOpen() const CATCH_OVERRIDE {
            return m_runState != NotStarted && !isComplete();
        }
        
        
        virtual void addChild( Ptr<ITracker> const& child ) CATCH_OVERRIDE {
            m_children.push_back( child );
        }
        
        virtual ITracker* findChild( std::string const& name ) CATCH_OVERRIDE {
            Children::const_iterator it = std::find_if( m_children.begin(), m_children.end(), TrackerHasName( name ) );
            return( it != m_children.end() )
                ? it->get()
                : CATCH_NULL;
        }
        virtual ITracker& parent() CATCH_OVERRIDE {
            assert( m_parent ); // Should always be non-null except for root
            return *m_parent;
        }
        
        virtual void openChild() CATCH_OVERRIDE {
            if( m_runState != ExecutingChildren ) {
                m_runState = ExecutingChildren;
                if( m_parent )
                    m_parent->openChild();
            }
        }
        void open() {
            m_runState = Executing;
            moveToThis();
            if( m_parent )
                m_parent->openChild();
        }
        
        virtual void close() CATCH_OVERRIDE {
            
            // Close any still open children (e.g. generators)
            while( &m_ctx.currentTracker() != this )
                m_ctx.currentTracker().close();
            
            switch( m_runState ) {
                case NotStarted:
                case CompletedSuccessfully:
                case Failed:
                    throw std::logic_error( "Illogical state" );
                    
                case NeedsAnotherRun:
                    break;;
                    
                case Executing:
                    m_runState = CompletedSuccessfully;
                    break;
                case ExecutingChildren:
                    if( m_children.empty() || m_children.back()->isComplete() )
                        m_runState = CompletedSuccessfully;
                    break;
                    
                default:
                    throw std::logic_error( "Unexpected state" );
            }
            moveToParent();
            m_ctx.completeCycle();
        }
        virtual void fail() CATCH_OVERRIDE {
            m_runState = Failed;
            if( m_parent )
                m_parent->markAsNeedingAnotherRun();
            moveToParent();
            m_ctx.completeCycle();
        }
        virtual void markAsNeedingAnotherRun() CATCH_OVERRIDE {
            m_runState = NeedsAnotherRun;
        }
    private:
        void moveToParent() {
            assert( m_parent );
            m_ctx.setCurrentTracker( m_parent );
        }
        void moveToThis() {
            m_ctx.setCurrentTracker( this );
        }
    };
    
    class SectionTracker : public TrackerBase {
    public:
        SectionTracker( std::string const& name, TrackerContext& ctx, ITracker* parent )
        :   TrackerBase( name, ctx, parent )
        {}
        virtual ~SectionTracker();
        
        static SectionTracker& acquire( TrackerContext& ctx, std::string const& name ) {
            SectionTracker* section = CATCH_NULL;
            
            ITracker& currentTracker = ctx.currentTracker();
            if( ITracker* childTracker = currentTracker.findChild( name ) ) {
                section = dynamic_cast<SectionTracker*>( childTracker );
                assert( section );
            }
            else {
                section = new SectionTracker( name, ctx, &currentTracker );
                currentTracker.addChild( section );
            }
            if( !ctx.completedCycle() && !section->isComplete() ) {
                
                section->open();
            }
            return *section;
        }
    };
    
    class IndexTracker : public TrackerBase {
        int m_size;
        int m_index;
    public:
        IndexTracker( std::string const& name, TrackerContext& ctx, ITracker* parent, int size )
        :   TrackerBase( name, ctx, parent ),
            m_size( size ),
            m_index( -1 )
        {}
        virtual ~IndexTracker();
        
        static IndexTracker& acquire( TrackerContext& ctx, std::string const& name, int size ) {
            IndexTracker* tracker = CATCH_NULL;
            
            ITracker& currentTracker = ctx.currentTracker();
            if( ITracker* childTracker = currentTracker.findChild( name ) ) {
                tracker = dynamic_cast<IndexTracker*>( childTracker );
                assert( tracker );
            }
            else {
                tracker = new IndexTracker( name, ctx, &currentTracker, size );
                currentTracker.addChild( tracker );
            }
            
            if( !ctx.completedCycle() && !tracker->isComplete() ) {
                if( tracker->m_runState != ExecutingChildren && tracker->m_runState != NeedsAnotherRun )
                    tracker->moveNext();
                tracker->open();
            }
            
            return *tracker;
        }
        
        int index() const { return m_index; }
        
        void moveNext() {
            m_index++;
            m_children.clear();
        }
        
        virtual void close() CATCH_OVERRIDE {
            TrackerBase::close();
            if( m_runState == CompletedSuccessfully && m_index < m_size-1 )
                m_runState = Executing;
        }
    };
    
    inline ITracker& TrackerContext::startRun() {
        m_rootTracker = new SectionTracker( "{root}", *this, CATCH_NULL );
        m_currentTracker = CATCH_NULL;
        m_runState = Executing;
        return *m_rootTracker;
    }
    
} // namespace TestCaseTracking
    
using TestCaseTracking::ITracker;
using TestCaseTracking::TrackerContext;
using TestCaseTracking::SectionTracker;
using TestCaseTracking::IndexTracker;
    
// !TBD: Deprecated
namespace SectionTracking {

    
    class TrackedSection {
        
        typedef std::map<std::string, TrackedSection> TrackedSections;
        
    public:
        enum RunState {
            NotStarted,
            Executing,
            ExecutingChildren,
            Completed
        };
        
        TrackedSection( std::string const& name, TrackedSection* parent )
        :   m_name( name ), m_runState( NotStarted ), m_parent( parent )
        {}
        
        RunState runState() const { return m_runState; }
        
        TrackedSection* findChild( std::string const& childName );
        TrackedSection* acquireChild( std::string const& childName );

        void enter() {
            if( m_runState == NotStarted )
                m_runState = Executing;
        }
        void leave();

        TrackedSection* getParent() {
            return m_parent;
        }
        bool hasChildren() const {
            return !m_children.empty();
        }
        
    private:
        std::string m_name;
        RunState m_runState;
        TrackedSections m_children;
        TrackedSection* m_parent;        
    };
    
    inline TrackedSection* TrackedSection::findChild( std::string const& childName ) {
        TrackedSections::iterator it = m_children.find( childName );
        return it != m_children.end()
            ? &it->second
            : CATCH_NULL;
    }
    inline TrackedSection* TrackedSection::acquireChild( std::string const& childName ) {
        if( TrackedSection* child = findChild( childName ) )
            return child;
        m_children.insert( std::make_pair( childName, TrackedSection( childName, this ) ) );
        return findChild( childName );
    }
    inline void TrackedSection::leave() {
        for( TrackedSections::const_iterator it = m_children.begin(), itEnd = m_children.end();
                it != itEnd;
                ++it )
            if( it->second.runState() != Completed ) {
                m_runState = ExecutingChildren;
                return;
            }
        m_runState = Completed;
    }

    class TestCaseTracker {
    public:
        TestCaseTracker( std::string const& testCaseName )
        :   m_testCase( testCaseName, CATCH_NULL ),
            m_currentSection( &m_testCase ),
            m_completedASectionThisRun( false )
        {}

        bool enterSection( std::string const& name ) {
            TrackedSection* child = m_currentSection->acquireChild( name );
            if( m_completedASectionThisRun || child->runState() == TrackedSection::Completed )
                return false;

            m_currentSection = child;
            m_currentSection->enter();
            return true;
        }
        void leaveSection() {
            m_currentSection->leave();
            m_currentSection = m_currentSection->getParent();
            assert( m_currentSection != CATCH_NULL );
            m_completedASectionThisRun = true;
        }

        bool currentSectionHasChildren() const {
            return m_currentSection->hasChildren();
        }
        bool isCompleted() const {
            return m_testCase.runState() == TrackedSection::Completed;
        }

        class Guard {
        public:
            Guard( TestCaseTracker& tracker ) : m_tracker( tracker ) {
                m_tracker.enterTestCase();
            }
            ~Guard() {
                m_tracker.leaveTestCase();
            }
        private:
            Guard( Guard const& );
            void operator = ( Guard const& );
            TestCaseTracker& m_tracker;
        };

    private:
        void enterTestCase() {
            m_currentSection = &m_testCase;
            m_completedASectionThisRun = false;
            m_testCase.enter();
        }
        void leaveTestCase() {
            m_testCase.leave();
        }

        TrackedSection m_testCase;
        TrackedSection* m_currentSection;
        bool m_completedASectionThisRun;
    };

} // namespace SectionTracking

using SectionTracking::TestCaseTracker;

} // namespace Catch

#endif // TWOBLUECUBES_CATCH_TEST_CASE_TRACKER_HPP_INCLUDED
