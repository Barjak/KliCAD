/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2023 Jon Evans <jon@craftyjon.com>
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <kinng.h>
#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>
#include <wx/log.h>


/**
 * Trace nng server debug output
 * @ingroup trace_env_vars
 */
static const wxChar TraceNng[] = wxT( "KINNG" );


// Per-context state.  Each context represents one concurrent
// request/reply pipeline on the shared REP socket; NNG enforces lockstep
// (send must follow recv) WITHIN a context, but contexts are independent.
struct KINNG_REQUEST_SERVER::ContextState
{
    enum class Phase
    {
        Idle,      // not yet started
        Recv,      // waiting for a request
        Handling,  // request received, user callback dispatched
        Send,      // sending the reply
    };

    nng_ctx ctx{};
    nng_aio* aio = nullptr;
    Phase phase = Phase::Idle;
    std::string sharedMessage;
    nng_msg* currentMsg = nullptr;  // owned during Handling/Send
    KINNG_REQUEST_SERVER* server = nullptr;
    int id = -1;
    std::mutex mutex;  // guards `phase` against concurrent Reply() races
};


KINNG_REQUEST_SERVER::KINNG_REQUEST_SERVER( const std::string& aSocketUrl, int aNumContexts ) :
        m_shutdown( false ),
        m_socketUrl( aSocketUrl ),
        m_numContexts( aNumContexts > 0 ? aNumContexts : 4 ),
        m_running( false )
{
}


KINNG_REQUEST_SERVER::~KINNG_REQUEST_SERVER()
{
    Stop();
}


bool KINNG_REQUEST_SERVER::Running() const
{
    return m_running.load();
}


bool KINNG_REQUEST_SERVER::Start()
{
    if( m_running.load() )
        return true;

    m_shutdown.store( false );

    nng_socket socket;
    int rv = nng_rep0_open( &socket );

    if( rv != 0 )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "nng_rep0_open failed: %d" ), rv ) );
        return false;
    }

    nng_listener listener;
    rv = nng_listener_create( &listener, socket, m_socketUrl.c_str() );

    if( rv != 0 )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "nng_listener_create failed: %d" ), rv ) );
        nng_close( socket );
        return false;
    }

    rv = nng_listener_start( listener, 0 );

    if( rv != 0 )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "nng_listener_start failed: %d" ), rv ) );
        nng_close( socket );
        return false;
    }

    m_socket = socket;

    m_contexts.clear();
    m_contexts.reserve( static_cast<size_t>( m_numContexts ) );

    for( int i = 0; i < m_numContexts; ++i )
    {
        auto state = std::make_unique<ContextState>();
        state->server = this;
        state->id = i;

        rv = nng_ctx_open( &state->ctx, socket );

        if( rv != 0 )
        {
            wxLogTrace( TraceNng,
                        wxString::Format( wxS( "nng_ctx_open[%d] failed: %d" ), i, rv ) );
            // Best effort: close any contexts already opened, then bail.
            for( auto& s : m_contexts )
            {
                if( s->aio )
                    nng_aio_free( s->aio );
                nng_ctx_close( s->ctx );
            }
            m_contexts.clear();
            nng_close( socket );
            return false;
        }

        rv = nng_aio_alloc( &state->aio, &KINNG_REQUEST_SERVER::aioCallback, state.get() );

        if( rv != 0 )
        {
            wxLogTrace( TraceNng,
                        wxString::Format( wxS( "nng_aio_alloc[%d] failed: %d" ), i, rv ) );
            nng_ctx_close( state->ctx );
            for( auto& s : m_contexts )
            {
                if( s->aio )
                    nng_aio_free( s->aio );
                nng_ctx_close( s->ctx );
            }
            m_contexts.clear();
            nng_close( socket );
            return false;
        }

        m_contexts.emplace_back( std::move( state ) );
    }

    m_running.store( true );

    // Kick off the first recv on every context.
    for( auto& s : m_contexts )
        startRecv( s.get() );

    wxLogTrace( TraceNng,
                wxString::Format( wxS( "KINNG_REQUEST_SERVER started with %d contexts" ),
                                  m_numContexts ) );
    return true;
}


void KINNG_REQUEST_SERVER::Stop()
{
    if( !m_running.load() )
        return;

    m_shutdown.store( true );

    // Cancel any in-flight aio operations on every context so the
    // callbacks unblock and the contexts can be closed cleanly.
    for( auto& s : m_contexts )
    {
        if( s->aio )
            nng_aio_cancel( s->aio );
    }

    // Wait for any pending aio callbacks to finish, then free.
    for( auto& s : m_contexts )
    {
        if( s->aio )
        {
            nng_aio_wait( s->aio );
            nng_aio_free( s->aio );
            s->aio = nullptr;
        }

        if( s->currentMsg )
        {
            nng_msg_free( s->currentMsg );
            s->currentMsg = nullptr;
        }

        nng_ctx_close( s->ctx );
    }

    m_contexts.clear();

    nng_close( m_socket );
    m_socket = nng_socket{};

    m_running.store( false );

    wxLogTrace( TraceNng, wxS( "KINNG_REQUEST_SERVER stopped" ) );
}


void KINNG_REQUEST_SERVER::startRecv( ContextState* aState )
{
    if( m_shutdown.load() )
        return;

    {
        std::lock_guard<std::mutex> lock( aState->mutex );
        aState->phase = ContextState::Phase::Recv;
    }

    nng_ctx_recv( aState->ctx, aState->aio );
}


void KINNG_REQUEST_SERVER::aioCallback( void* aArg )
{
    auto* state = static_cast<ContextState*>( aArg );

    if( state && state->server )
        state->server->onAioReady( state );
}


void KINNG_REQUEST_SERVER::onAioReady( ContextState* aState )
{
    if( m_shutdown.load() )
        return;

    int rv = nng_aio_result( aState->aio );

    ContextState::Phase phase;
    {
        std::lock_guard<std::mutex> lock( aState->mutex );
        phase = aState->phase;
    }

    if( phase == ContextState::Phase::Recv )
    {
        if( rv != 0 )
        {
            // Recv errored (cancellation on shutdown, or transport
            // error).  If not shutting down, restart the recv so the
            // context stays live.
            if( !m_shutdown.load() )
                startRecv( aState );
            return;
        }

        nng_msg* msg = nng_aio_get_msg( aState->aio );
        aState->currentMsg = msg;
        aState->sharedMessage.assign( static_cast<char*>( nng_msg_body( msg ) ),
                                      nng_msg_len( msg ) );

        {
            std::lock_guard<std::mutex> lock( aState->mutex );
            aState->phase = ContextState::Phase::Handling;
        }

        // Dispatch to user.  The user is expected to call Reply(reply, ctx_id)
        // either inline on this thread (for fast/control-plane handlers) or
        // later from another thread (for slow handlers that wxQueueEvent the
        // work onto the main loop).
        if( m_callback )
            m_callback( &aState->sharedMessage, aState->id );

        return;
    }

    if( phase == ContextState::Phase::Send )
    {
        // Send completed — free the message we sent and re-arm recv.
        if( aState->currentMsg )
        {
            // After nng_aio_set_msg + nng_ctx_send, NNG takes ownership of
            // the msg on success and frees it for us.  On failure we'd need
            // to free.  Distinguish via nng_aio_result.
            if( rv != 0 )
            {
                // Send failed; we still own the msg.  Free it.
                nng_msg_free( aState->currentMsg );
            }
            aState->currentMsg = nullptr;
        }

        if( !m_shutdown.load() )
            startRecv( aState );
        return;
    }

    // Idle / unexpected.  Ignore.
}


void KINNG_REQUEST_SERVER::Reply( const std::string& aReply, int aCtxId )
{
    if( aCtxId < 0 || static_cast<size_t>( aCtxId ) >= m_contexts.size() )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "Reply: invalid ctx id %d" ), aCtxId ) );
        return;
    }

    ContextState* state = m_contexts[aCtxId].get();

    nng_msg* msg = nullptr;
    int rv = nng_msg_alloc( &msg, 0 );

    if( rv != 0 )
    {
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "nng_msg_alloc failed: %d" ), rv ) );
        return;
    }

    rv = nng_msg_append( msg, aReply.data(), aReply.size() );

    if( rv != 0 )
    {
        nng_msg_free( msg );
        wxLogTrace( TraceNng,
                    wxString::Format( wxS( "nng_msg_append failed: %d" ), rv ) );
        return;
    }

    {
        std::lock_guard<std::mutex> lock( state->mutex );
        // The prior `currentMsg` from the recv is owned by us — free it now
        // that we're done with it.  (We copied its body into sharedMessage
        // when the recv aio fired.)
        if( state->currentMsg )
        {
            nng_msg_free( state->currentMsg );
            state->currentMsg = nullptr;
        }

        state->currentMsg = msg;
        state->phase = ContextState::Phase::Send;
    }

    nng_aio_set_msg( state->aio, msg );
    nng_ctx_send( state->ctx, state->aio );
}
