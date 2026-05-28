/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2024 Jon Evans <jon@craftyjon.com>
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

#ifndef KICAD_API_SERVER_H
#define KICAD_API_SERVER_H

#include <atomic>
#include <memory>
#include <set>
#include <string>

#include <wx/event.h>
#include <wx/filename.h>

#include <kicommon.h>

class API_HANDLER;
class API_HANDLER_PYTHON;
class EMBEDDED_PYTHON;
class KINNG_REQUEST_SERVER;
class wxEvtHandler;


wxDECLARE_EVENT( API_REQUEST_EVENT, wxCommandEvent );


class KICOMMON_API KICAD_API_SERVER : public wxEvtHandler
{
public:
    KICAD_API_SERVER( bool aAutoStart = true );

    ~KICAD_API_SERVER();

    void Start();

    void Stop();

    bool Running() const;

    /**
     * Adds a new request handler to the server.  Each handler maintains its own list of API
     * messages that it knows how to handle, and the server will pass every incoming message to all
     * handlers in succession until one of them handles it.
     *
     * The caller is responsible for the lifetime of the handler and must call DeregisterHandler
     * before the pointer is freed.
     *
     * @param aHandler is a pointer (non-owned) to API_HANDLER
     */
    void RegisterHandler( API_HANDLER* aHandler );

    void DeregisterHandler( API_HANDLER* aHandler );

    void SetReadyToReply( bool aReady = true )
    {
        m_readyToReply.store( aReady, std::memory_order_release );
    }

    void SetSocketPath( const wxString& aSocketPath )
    {
        m_socketPathOverride = aSocketPath;
    }

    std::string SocketPath() const;

    const std::string& Token() const { return m_token; }

private:

    /**
     * Callback that executes on an NNG worker thread.  For the control-plane
     * sentinel (see CONTROL_DISMISS_MODAL in api_server.cpp), handles inline
     * and Reply()s synchronously.  Otherwise generates a wxCommandEvent
     * (carrying the request bytes + ctxId) that will be handled by the wx
     * event loop later.
     *
     * @param aRequest is a pointer to a string containing bytes that came in over the wire
     * @param aCtxId   is the KINNG context id; must be passed back to Reply()
     */
    void onApiRequest( std::string* aRequest, int aCtxId );

    /**
     * Event handler that receives the event on the main thread sent by onApiRequest
     * @param aEvent will contain a pointer to an incoming API request string in the client data,
     *               and the ctxId via aEvent.GetInt().
     */
    void handleApiEvent( wxCommandEvent& aEvent );

    void handleApiRequestString( std::string& aRequestString, int aCtxId );

    void log( const std::string& aOutput );

    std::unique_ptr<KINNG_REQUEST_SERVER> m_server;

    // Local fork: embedded Python interpreter + its IPC handler, both owned
    // by the server so they share its lifetime.  Initialized in Start(),
    // torn down in Stop().
    std::unique_ptr<EMBEDDED_PYTHON>      m_embeddedPython;
    std::unique_ptr<API_HANDLER_PYTHON>   m_pythonHandler;

    std::set<API_HANDLER*> m_handlers;

    std::string m_token;

    std::atomic<bool> m_readyToReply;

    wxString m_socketPathOverride;

    static wxString s_logFileName;

    wxFileName m_logFilePath;
};

#endif //KICAD_API_SERVER_H
