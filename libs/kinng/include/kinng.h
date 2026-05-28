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

#ifndef KICAD_KINNG_H
#define KICAD_KINNG_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>


#include <nng/nng.h>


/**
 * REQ/REP server with multiple concurrent contexts.
 *
 * Each context (`nng_ctx`) is an independent request/reply pipeline on the
 * shared REP socket.  This lets the server have N requests in flight
 * simultaneously without violating NNG's per-context lockstep — different
 * clients (or different roles from the same client) can each occupy their own
 * context.
 *
 * The motivating use case is modal-dialog dismissal in the embedded IPC
 * server: when one context is parked waiting for the wx main loop to dispatch
 * a request (which is blocked because a modal dialog has suspended the loop),
 * a *second* context can still receive a "dismiss-modal" request, handle it
 * inline on an NNG worker thread, and reply immediately.  Without multiple
 * contexts, the listener would be stuck in `nng_recv` and the dismiss request
 * would never be received.
 *
 * The user callback is invoked when a request lands on any context.  The
 * `int ctxId` argument tells the user which context the request came in on;
 * the same id must be passed to `Reply()` so the response routes back to the
 * originating client.
 *
 * The callback is called on an NNG worker thread, NOT the thread that
 * constructed this server.  All work the callback does must be thread-safe
 * with respect to any other code running concurrently.  In particular, the
 * existing pattern of `wxQueueEvent`-ing into the wx main loop is fine
 * (wxQueueEvent is documented thread-safe).
 *
 * Reply may be called from any thread.  It does not block waiting for the
 * send to complete; the send completes asynchronously and a new recv is
 * automatically started on that context.
 */
class KINNG_REQUEST_SERVER
{
public:
    KINNG_REQUEST_SERVER( const std::string& aSocketUrl, int aNumContexts = 4 );

    ~KINNG_REQUEST_SERVER();

    bool Start();

    void Stop();

    bool Running() const;

    void SetCallback( std::function<void(std::string*, int)> aFunc )
    {
        m_callback = std::move( aFunc );
    }

    /// Send a reply on the given context.  ``aCtxId`` must match the value
    /// passed to the callback for the request being replied to.  May be called
    /// from any thread; non-blocking.
    void Reply( const std::string& aReply, int aCtxId );

    const std::string& SocketPath() const { return m_socketUrl; }

private:
    struct ContextState;

    static void aioCallback( void* aArg );
    void onAioReady( ContextState* aState );
    void startRecv( ContextState* aState );

    std::atomic<bool> m_shutdown;

    std::string m_socketUrl;

    std::function<void(std::string*, int)> m_callback;

    int m_numContexts;

    std::vector<std::unique_ptr<ContextState>> m_contexts;

    // The shared REP socket on which all contexts operate.
    nng_socket m_socket{};

    std::atomic<bool> m_running;

    std::mutex m_replyMutex;
};

#endif //KICAD_KINNG_H
