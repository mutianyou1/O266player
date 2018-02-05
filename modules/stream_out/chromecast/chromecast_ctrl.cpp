/*****************************************************************************
 * chromecast_ctrl.cpp: Chromecast module for vlc
 *****************************************************************************
 * Copyright © 2014-2015 VideoLAN
 *
 * Authors: Adrien Maglo <magsoft@videolan.org>
 *          Jean-Baptiste Kempf <jb@videolan.org>
 *          Steve Lhomme <robux4@videolabs.io>
 *          Hugo Beauzée-Luyssen <hugo@beauzee.fr>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "chromecast.h"

#include <cassert>
#include <cerrno>

#include <vlc_stream.h>

#include "../../misc/webservices/json.h"

/* deadline regarding pings sent from receiver */
#define PING_WAIT_TIME 6000
#define PING_WAIT_RETRIES 1

static const mtime_t SEEK_FORWARD_OFFSET = 1000000;

static const char* StateToStr( States s )
{
    switch (s )
    {
    case Authenticating:
        return "Authenticating";
    case Connecting:
        return "Connecting";
    case Connected:
        return "Connected";
    case Launching:
        return "Lauching";
    case Ready:
        return "Ready";
    case LoadFailed:
        return "LoadFailed";
    case Loading:
        return "Loading";
    case Buffering:
        return "Buffering";
    case Playing:
        return "Playing";
    case Paused:
        return "Paused";
    case Seeking:
        return "Seeking";
    case Stopping:
        return "Stopping";
    case Dead:
        return "Dead";
    case TakenOver:
        return "TakenOver";
    }
    vlc_assert_unreachable();
}

/*****************************************************************************
 * intf_sys_t: class definition
 *****************************************************************************/
intf_sys_t::intf_sys_t(vlc_object_t * const p_this, int port, std::string device_addr,
                       int device_port, vlc_interrupt_t *p_interrupt, httpd_host_t *httpd_host)
 : m_module(p_this)
 , m_streaming_port(port)
 , m_mediaSessionId( 0 )
 , m_communication( p_this, device_addr.c_str(), device_port )
 , m_state( Authenticating )
 , m_eof( false )
 , m_meta( NULL )
 , m_ctl_thread_interrupt(p_interrupt)
 , m_httpd_host(httpd_host)
 , m_httpd_file(NULL)
 , m_art_url(NULL)
 , m_art_stream(NULL)
 , m_time_playback_started( VLC_TS_INVALID )
 , m_ts_local_start( VLC_TS_INVALID )
 , m_length( VLC_TS_INVALID )
 , m_pingRetriesLeft( PING_WAIT_RETRIES )
{
    vlc_mutex_init(&m_lock);
    vlc_cond_init( &m_stateChangedCond );

    std::stringstream ss;
    ss << "http://" << m_communication.getServerIp() << ":" << port;
    m_art_http_ip = ss.str();

    m_common.p_opaque = this;
    m_common.pf_get_position     = get_position;
    m_common.pf_get_time         = get_time;
    m_common.pf_set_length       = set_length;
    m_common.pf_set_initial_time = set_initial_time;
    m_common.pf_wait_app_started = wait_app_started;
    m_common.pf_request_seek     = request_seek;
    m_common.pf_wait_seek_done   = wait_seek_done;
    m_common.pf_set_pause_state  = set_pause_state;
    m_common.pf_set_meta         = set_meta;

    assert( var_Type( m_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME) == 0 );
    if (var_Create( m_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME, VLC_VAR_ADDRESS ) == VLC_SUCCESS )
        var_SetAddress( m_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME, &m_common );

    // Start the Chromecast event thread.
    if (vlc_clone(&m_chromecastThread, ChromecastThread, this,
                  VLC_THREAD_PRIORITY_LOW))
        throw std::runtime_error( "error creating cc thread" );
}

intf_sys_t::~intf_sys_t()
{
    var_Destroy( m_module->obj.parent->obj.parent, CC_SHARED_VAR_NAME );

    vlc_mutex_lock(&m_lock);
    switch ( m_state )
    {
    case Ready:
    case Loading:
    case Buffering:
    case Playing:
    case Paused:
    case Seeking:
    case Stopping:
        // Generate the close messages.
        m_communication.msgReceiverClose( m_appTransportId );
        /* fallthrough */
    case Connecting:
    case Connected:
    case Launching:
        m_communication.msgReceiverClose(DEFAULT_CHOMECAST_RECEIVER);
        /* fallthrough */
    default:
        break;
    }
    vlc_mutex_unlock(&m_lock);

    vlc_interrupt_kill( m_ctl_thread_interrupt );

    vlc_join(m_chromecastThread, NULL);

    vlc_interrupt_destroy( m_ctl_thread_interrupt );

    if (m_meta != NULL)
        vlc_meta_Delete(m_meta);

    if( m_httpd_file )
        httpd_FileDelete( m_httpd_file );
    if( m_art_stream )
        vlc_stream_Delete( m_art_stream );

    vlc_cond_destroy(&m_stateChangedCond);
    vlc_mutex_destroy(&m_lock);
}

int intf_sys_t::httpd_file_fill( uint8_t *psz_request, uint8_t **pp_data, int *pi_data )
{
    (void) psz_request;

    if( vlc_stream_Seek( m_art_stream, 0 ) )
        return VLC_EGENERIC;

    uint64_t size;
    if( vlc_stream_GetSize( m_art_stream, &size ) != VLC_SUCCESS
     || size > INT64_C( 10000000 ) )
        return VLC_EGENERIC;

    *pp_data = (uint8_t *)malloc( size );
    if( !*pp_data )
        return VLC_EGENERIC;

    ssize_t read = vlc_stream_Read( m_art_stream, *pp_data, size );
    if( read < 0 || (size_t)read != size )
    {
        free( *pp_data );
        *pp_data = NULL;
        return VLC_EGENERIC;
    }
    *pi_data = size;

    return VLC_SUCCESS;
}

static int httpd_file_fill_cb( httpd_file_sys_t *data, httpd_file_t *http_file,
                          uint8_t *psz_request, uint8_t **pp_data, int *pi_data )
{
    (void) http_file;
    intf_sys_t *p_sys = static_cast<intf_sys_t*>((void *)data);
    return p_sys->httpd_file_fill( psz_request, pp_data, pi_data );
}

void intf_sys_t::prepareHttpArtwork()
{
    const char *psz_art = m_meta ? vlc_meta_Get( m_meta, vlc_meta_ArtworkURL ) : NULL;
    /* Abort if there is no art or if the art is already served */
    if( !psz_art || strncmp( psz_art, "http", 4) == 0
     || ( m_art_url && strcmp( psz_art, m_art_url ) == 0 ) )
        return;

    if( m_httpd_file )
    {
        httpd_FileDelete( m_httpd_file );
        m_httpd_file = NULL;
    }
    if( m_art_stream )
    {
        vlc_stream_Delete( m_art_stream );
        m_art_stream = NULL;
    }

    m_art_stream = vlc_stream_NewURL( m_module, psz_art );
    if( !m_art_stream )
        return;

    uint64_t size;
    if( vlc_stream_GetSize( m_art_stream, &size ) != VLC_SUCCESS
     || size > INT64_C( 10000000 ) )
    {
        msg_Warn( m_module, "art stream is too big or invalid" );
        vlc_stream_Delete( m_art_stream );
        return;
    }

    const char *psz_artmime = "application/octet-stream";
    char *psz_streammime = stream_MimeType( m_art_stream );
    if( psz_streammime )
        psz_artmime = psz_streammime;

    m_httpd_file = httpd_FileNew( m_httpd_host, "/art", psz_artmime, NULL, NULL,
                                  httpd_file_fill_cb, (httpd_file_sys_t *) this );
    free( psz_streammime );
    if( !m_httpd_file )
    {
        vlc_stream_Delete( m_art_stream );
        return;
    }

    std::stringstream ss;
    ss << m_art_http_ip << "/art";
    vlc_meta_Set( m_meta, vlc_meta_ArtworkURL, ss.str().c_str() );
}

void intf_sys_t::setHasInput( const std::string mime_type )
{
    vlc_mutex_locker locker(&m_lock);
    msg_Dbg( m_module, "Loading content" );

    this->m_mime = mime_type;

    /* new input: clear message queue */
    std::queue<QueueableMessages> empty;
    std::swap(m_msgQueue, empty);

    waitAppStarted();
    if ( m_state == Dead )
    {
        msg_Warn( m_module, "no Chromecast hook possible");
        return;
    }

    prepareHttpArtwork();

    // We should now be in the ready state, and therefor have a valid transportId
    assert( m_appTransportId.empty() == false );
    // Reset the mediaSessionID to allow the new session to become the current one.
    m_mediaSessionId = 0;
    // we cannot start a new load when the last one is still processing
    m_communication.msgPlayerLoad( m_appTransportId, m_streaming_port, mime_type, m_meta );
    setState( Loading );
    m_eof = false;
}

/**
 * @brief Process a message received from the Chromecast
 * @param msg the CastMessage to process
 * @return 0 if the message has been successfuly processed else -1
 */
void intf_sys_t::processMessage(const castchannel::CastMessage &msg)
{
    const std::string & namespace_ = msg.namespace_();

#ifndef NDEBUG
    msg_Dbg( m_module, "processMessage: %s->%s %s", namespace_.c_str(), msg.destination_id().c_str(), msg.payload_utf8().c_str());
#endif

    if (namespace_ == NAMESPACE_DEVICEAUTH)
        processAuthMessage( msg );
    else if (namespace_ == NAMESPACE_HEARTBEAT)
        processHeartBeatMessage( msg );
    else if (namespace_ == NAMESPACE_RECEIVER)
        processReceiverMessage( msg );
    else if (namespace_ == NAMESPACE_MEDIA)
        processMediaMessage( msg );
    else if (namespace_ == NAMESPACE_CONNECTION)
        processConnectionMessage( msg );
    else
    {
        msg_Err( m_module, "Unknown namespace: %s", msg.namespace_().c_str());
    }
}

void intf_sys_t::queueMessage( QueueableMessages msg )
{
    // Assume lock is held by the called
    m_msgQueue.push( msg );
    vlc_interrupt_raise( m_ctl_thread_interrupt );
}



/*****************************************************************************
 * Chromecast thread
 *****************************************************************************/
void* intf_sys_t::ChromecastThread(void* p_data)
{
    intf_sys_t *p_sys = static_cast<intf_sys_t*>(p_data);
    p_sys->mainLoop();
    return NULL;
}

void intf_sys_t::mainLoop()
{
    vlc_savecancel();
    vlc_interrupt_set( m_ctl_thread_interrupt );

    // State was already initialized as Authenticating
    m_communication.msgAuth();

    while ( !vlc_killed() )
    {
        if ( !handleMessages() )
            break;
        // Reset the interrupt state to avoid commands not being sent (since
        // the context is still flagged as interrupted)
        vlc_interrupt_unregister();
        vlc_mutex_locker lock( &m_lock );
        while ( m_msgQueue.empty() == false )
        {
            QueueableMessages msg = m_msgQueue.front();
            switch ( msg )
            {
                case Stop:
                    m_communication.msgPlayerStop( m_appTransportId, m_mediaSessionId );
                    setState( Stopping );
                    break;
                case Seek:
                {
                    char current_time[32];
                    mtime_t seek_request_time = mdate() + SEEK_FORWARD_OFFSET;
                    if( snprintf( current_time, sizeof(current_time), "%.3f",
                                  double( seek_request_time ) / 1000000.0 ) >= (int)sizeof(current_time) )
                    {
                        msg_Err( m_module, "snprintf() truncated string for mediaSessionId" );
                        current_time[sizeof(current_time) - 1] = '\0';
                    }
                    /* send a fake time to seek to, to make sure the device flushes its buffers */
                    m_communication.msgPlayerSeek( m_appTransportId, m_mediaSessionId, current_time );
                    setState( Seeking );
                    break;
                }
            }
            m_msgQueue.pop();
        }
    }
}

void intf_sys_t::processAuthMessage( const castchannel::CastMessage& msg )
{
    castchannel::DeviceAuthMessage authMessage;
    if ( authMessage.ParseFromString(msg.payload_binary()) == false )
    {
        msg_Warn( m_module, "Failed to parse the payload" );
        return;
    }

    if (authMessage.has_error())
    {
        msg_Err( m_module, "Authentification error: %d", authMessage.error().error_type());
    }
    else if (!authMessage.has_response())
    {
        msg_Err( m_module, "Authentification message has no response field");
    }
    else
    {
        vlc_mutex_locker locker(&m_lock);
        setState( Connecting );
        m_communication.msgConnect(DEFAULT_CHOMECAST_RECEIVER);
        m_communication.msgReceiverGetStatus();
    }
}

void intf_sys_t::processHeartBeatMessage( const castchannel::CastMessage& msg )
{
    json_value *p_data = json_parse(msg.payload_utf8().c_str());
    std::string type((*p_data)["type"]);

    if (type == "PING")
    {
        msg_Dbg( m_module, "PING received from the Chromecast");
        m_communication.msgPong();
    }
    else if (type == "PONG")
    {
        msg_Dbg( m_module, "PONG received from the Chromecast");
        m_pingRetriesLeft = PING_WAIT_RETRIES;
    }
    else
    {
        msg_Warn( m_module, "Heartbeat command not supported: %s", type.c_str());
    }

    json_value_free(p_data);
}

void intf_sys_t::processReceiverMessage( const castchannel::CastMessage& msg )
{
    json_value *p_data = json_parse(msg.payload_utf8().c_str());
    std::string type((*p_data)["type"]);

    if (type == "RECEIVER_STATUS")
    {
        json_value applications = (*p_data)["status"]["applications"];
        const json_value *p_app = NULL;

        for (unsigned i = 0; i < applications.u.array.length; ++i)
        {
            if ( strcmp( applications[i]["appId"], APP_ID ) == 0 )
            {
                if ( (const char*)applications[i]["transportId"] != NULL)
                {
                    p_app = &applications[i];
                    break;
                }
            }
        }

        vlc_mutex_locker locker(&m_lock);

        switch ( m_state )
        {
        case Connecting:
            // We were connecting & fetching the current status.
            // The media receiver app is running, we are ready to proceed
            if ( p_app != NULL )
            {
                msg_Dbg( m_module, "Media receiver application was already running" );
                m_appTransportId = (const char*)(*p_app)["transportId"];
                setState( Ready );
                m_communication.msgConnect( m_appTransportId );
            }
            else
            {
                setState( Connected );
            }
            break;
        case Launching:
            // We already asked for the media receiver application to start
            if ( p_app != NULL )
            {
                msg_Dbg( m_module, "Media receiver application has been started." );
                setState( Ready );
                m_appTransportId = (const char*)(*p_app)["transportId"];
                m_communication.msgConnect( m_appTransportId );
            }
            break;
        case Loading:
        case Playing:
        case Paused:
        case Seeking:
        case Ready:
        case TakenOver:
            if ( p_app == NULL )
            {
                msg_Warn( m_module, "Media receiver application got closed." );
                setState( Connected );
                m_appTransportId = "";
                m_mediaSessionId = 0;
            }
            break;
        case Connected:
            // We might receive a RECEIVER_STATUS while being connected, when pinging/asking the status
            if ( p_app == NULL )
                break;
            // else: fall through and warn
        default:
            msg_Warn( m_module, "Unexpected RECEIVER_STATUS with state %s. "
                      "Checking media status",
                      StateToStr( m_state ) );
            // This is likely because the chromecast refused the playback, but
            // let's check by explicitely probing the media status
            m_communication.msgPlayerGetStatus( m_appTransportId );
            break;
        }
    }
    else if (type == "LAUNCH_ERROR")
    {
        json_value reason = (*p_data)["reason"];
        msg_Err( m_module, "Failed to start the MediaPlayer: %s",
                (const char *)reason);
        vlc_mutex_locker locker(&m_lock);
        setState( Dead );
    }
    else
    {
        msg_Warn( m_module, "Receiver command not supported: %s",
                msg.payload_utf8().c_str());
    }

    json_value_free(p_data);
}

void intf_sys_t::processMediaMessage( const castchannel::CastMessage& msg )
{
    json_value *p_data = json_parse(msg.payload_utf8().c_str());
    std::string type((*p_data)["type"]);

    if (type == "MEDIA_STATUS")
    {
        json_value status = (*p_data)["status"];

        int64_t sessionId = (json_int_t) status[0]["mediaSessionId"];
        if (m_mediaSessionId != sessionId && m_mediaSessionId != 0 )
        {
            msg_Dbg( m_module, "Ignoring message for a different media session" );
            json_value_free(p_data);
            return;
        }

        msg_Dbg( m_module, "Player state: %s sessionId: %" PRId64,
                status[0]["playerState"].operator const char *(),
                sessionId );

        std::string newPlayerState = (const char*)status[0]["playerState"];
        std::string idleReason = (const char*)status[0]["idleReason"];

        vlc_mutex_locker locker( &m_lock );

        if (newPlayerState == "IDLE" || newPlayerState.empty() == true )
        {
            /* Idle state is expected when the media receiver application is
             * started. In case the state is still Buffering, it denotes an error.
             * In most case, we'd receive a RECEIVER_STATUS message, which causes
             * use to ask for the MEDIA_STATUS before assuming an error occured.
             * If the chromecast silently gave up on playing our stream, we also
             * might have an empty status array.
             * If the media load indeed failed, we need to try another
             * transcode/remux configuration, or give up.
             * In case we are now loading, we might also receive an INTERRUPTED
             * state for the previous session, which we wouldn't ignore earlier
             * since our mediaSessionID was reset to 0.
             * In this case, don't assume we're being taken over, as we are
             * actually doing the take over.
             */
            if ( m_state != Ready && m_state != LoadFailed && m_state != Loading )
            {
                // The playback stopped
                m_time_playback_started = VLC_TS_INVALID;
                if ( idleReason == "INTERRUPTED" )
                {
                    setState( TakenOver );
                    // Do not reset the mediaSessionId to ensure we refuse all
                    // other MEDIA_STATUS from the new session.
                }
                else if ( m_state == Buffering )
                    setState( LoadFailed );
                else
                {
                    if (idleReason == "FINISHED")
                        m_eof = true;
                    setState( Ready );
                }
            }
        }
        else
        {
            if ( m_mediaSessionId == 0 )
            {
                m_mediaSessionId = sessionId;
                msg_Dbg( m_module, "New mediaSessionId: %" PRId64, m_mediaSessionId );
            }

            if (newPlayerState == "PLAYING")
            {
                msg_Dbg( m_module, "Playback started now:%" PRId64 " i_ts_local_start:%" PRId64,
                         m_time_playback_started, m_ts_local_start);
                if ( m_state != Playing )
                {
                    /* TODO reset demux PCR ? */
                    m_time_playback_started = mdate();
                    setState( Playing );
                }
            }
            else if (newPlayerState == "BUFFERING")
            {
                if ( m_state != Buffering )
                {
                    /* EOF when state goes from Playing to Buffering. There can
                     * be a lot of false positives (when seeking or when the cc
                     * request more input) but this state is fetched only when
                     * the input has reached EOF. */

                    m_time_playback_started = VLC_TS_INVALID;
                    setState( Buffering );
                }
            }
            else if (newPlayerState == "PAUSED")
            {
                if ( m_state != Paused )
                {
    #ifndef NDEBUG
                    msg_Dbg( m_module, "Playback paused: date_play_start: %" PRId64, m_time_playback_started);
    #endif

                    if ( m_time_playback_started != VLC_TS_INVALID && m_state == Playing )
                    {
                        /* this is a pause generated remotely, adjust the playback time */
                        m_ts_local_start += mdate() - m_time_playback_started;
    #ifndef NDEBUG
                        msg_Dbg( m_module, "updated i_ts_local_start:%" PRId64, m_ts_local_start);
    #endif
                    }
                    m_time_playback_started = VLC_TS_INVALID;
                    setState( Paused );
                }
            }
            else if ( newPlayerState == "LOADING" )
            {
                if ( m_state != Loading )
                {
                    msg_Dbg( m_module, "Chromecast is loading the stream" );
                    setState( Loading );
                }
            }
            else
                msg_Warn( m_module, "Unknown Chromecast MEDIA_STATUS state %s", newPlayerState.c_str());
        }
    }
    else if (type == "LOAD_FAILED")
    {
        msg_Err( m_module, "Media load failed");
        vlc_mutex_locker locker(&m_lock);
        setState( LoadFailed );
    }
    else if (type == "LOAD_CANCELLED")
    {
        msg_Dbg( m_module, "LOAD canceled by another command");
    }
    else if (type == "INVALID_REQUEST")
    {
        msg_Dbg( m_module, "We sent an invalid request reason:%s", (const char*)(*p_data)["reason"] );
    }
    else
    {
        msg_Warn( m_module, "Media command not supported: %s",
                msg.payload_utf8().c_str());
    }

    json_value_free(p_data);
}

void intf_sys_t::processConnectionMessage( const castchannel::CastMessage& msg )
{
    json_value *p_data = json_parse(msg.payload_utf8().c_str());
    std::string type((*p_data)["type"]);
    json_value_free(p_data);

    if ( type == "CLOSE" )
    {
        // Close message indicates an application is being closed, not the connection.
        // From this point on, we need to relaunch the media receiver app
        vlc_mutex_locker locker(&m_lock);
        m_appTransportId = "";
        m_mediaSessionId = 0;
        setState( Connected );
    }
    else
    {
        msg_Warn( m_module, "Connection command not supported: %s",
                type.c_str());
    }
}

bool intf_sys_t::handleMessages()
{
    uint8_t p_packet[PACKET_MAX_LEN];
    size_t i_payloadSize = 0;
    size_t i_received = 0;
    bool b_timeout = false;
    mtime_t i_begin_time = mdate();

    /* Packet structure:
     * +------------------------------------+------------------------------+
     * | Payload size (uint32_t big endian) |         Payload data         |
     * +------------------------------------+------------------------------+
     */
    while ( true )
    {
        // If we haven't received the payload size yet, let's wait for it. Otherwise, we know
        // how many bytes to read
        ssize_t i_ret = m_communication.receive( p_packet + i_received,
                                        i_payloadSize + PACKET_HEADER_LEN - i_received,
                                        PING_WAIT_TIME - ( mdate() - i_begin_time ) / CLOCK_FREQ,
                                        &b_timeout );
        if ( i_ret < 0 )
        {
            if ( errno == EINTR )
                return true;
            // An error occured, we give up
            msg_Err( m_module, "The connection to the Chromecast died (receiving).");
            vlc_mutex_locker locker(&m_lock);
            setState( Dead );
            return false;
        }
        else if ( b_timeout == true )
        {
            // If no commands were queued to be sent, we timed out. Let's ping the chromecast
            if ( m_pingRetriesLeft == 0 )
            {
                vlc_mutex_locker locker(&m_lock);
                m_state = Dead;
                msg_Warn( m_module, "No PING response from the chromecast" );
                return false;
            }
            --m_pingRetriesLeft;
            m_communication.msgPing();
            m_communication.msgReceiverGetStatus();
            return true;
        }
        assert( i_ret != 0 );
        i_received += i_ret;
        if ( i_payloadSize == 0 )
        {
            i_payloadSize = U32_AT( p_packet );
            if ( i_payloadSize > PACKET_MAX_LEN - PACKET_HEADER_LEN )
            {
                msg_Err( m_module, "Payload size is too long: dropping connection" );
                vlc_mutex_locker locker(&m_lock);
                m_state = Dead;
                return false;
            }
            continue;
        }
        assert( i_received <= i_payloadSize + PACKET_HEADER_LEN );
        if ( i_received == i_payloadSize + PACKET_HEADER_LEN )
            break;
    }
    castchannel::CastMessage msg;
    msg.ParseFromArray(p_packet + PACKET_HEADER_LEN, i_payloadSize);
    processMessage(msg);
    return true;
}

void intf_sys_t::requestPlayerStop()
{
    vlc_mutex_locker locker(&m_lock);

    if( m_httpd_file )
    {
        httpd_FileDelete( m_httpd_file );
        m_httpd_file = NULL;
    }
    if( m_art_stream )
    {
        vlc_stream_Delete( m_art_stream );
        m_art_stream = NULL;
    }

    if ( m_mediaSessionId == 0 || m_state == TakenOver )
        return;
    queueMessage( Stop );
}

States intf_sys_t::state() const
{
    vlc_mutex_locker locker( &m_lock );
    return m_state;
}

void intf_sys_t::requestPlayerSeek(mtime_t pos)
{
    vlc_mutex_locker locker(&m_lock);
    if ( m_mediaSessionId == 0 )
        return;
    if ( pos != VLC_TS_INVALID )
        m_ts_local_start = pos;
    queueMessage( Seek );
}

void intf_sys_t::setPauseState(bool paused)
{
    msg_Dbg( m_module, "%s state", paused ? "paused" : "playing" );
    vlc_mutex_locker locker( &m_lock );
    if ( !paused )
    {
        if ( m_mediaSessionId != 0 )
        {
            m_communication.msgPlayerPlay( m_appTransportId, m_mediaSessionId );
        }
    }
    else
    {
        if ( m_mediaSessionId != 0 && m_state != Paused )
        {
            m_communication.msgPlayerPause( m_appTransportId, m_mediaSessionId );
        }
    }
}

void intf_sys_t::waitAppStarted()
{
    while ( m_state == Connected || m_state == Launching ||
            m_state == Authenticating || m_state == Connecting ||
            m_state == Stopping )
    {
        if ( m_state == Connected )
        {
            msg_Dbg( m_module, "Starting the media receiver application" );
            // Don't use setState as we don't want to signal the condition in this case.
            m_state = Launching;
            m_communication.msgReceiverLaunchApp();
        }
        msg_Dbg( m_module, "Waiting for Chromecast media receiver app to be ready" );
        vlc_cond_wait(&m_stateChangedCond, &m_lock);
    }
    msg_Dbg( m_module, "Done waiting for application. transportId: %s", m_appTransportId.c_str() );
}

void intf_sys_t::waitSeekDone()
{
    vlc_mutex_locker locker(&m_lock);
    while ( m_state == Seeking )
    {
#ifndef NDEBUG
        msg_Dbg( m_module, "waiting for Chromecast seek" );
#endif
        vlc_cond_wait(&m_stateChangedCond, &m_lock);
#ifndef NDEBUG
        msg_Dbg( m_module, "finished waiting for Chromecast seek" );
#endif
    }
}

bool intf_sys_t::isFinishedPlaying()
{
    vlc_mutex_locker locker(&m_lock);
    return m_state == LoadFailed || m_state == Dead || m_eof;
}

void intf_sys_t::setMeta(vlc_meta_t *p_meta)
{
    vlc_mutex_locker locker(&m_lock);
    if (m_meta != NULL)
        vlc_meta_Delete(m_meta);
    m_meta = p_meta;
}

mtime_t intf_sys_t::getPlaybackTimestamp() const
{
    switch( m_state )
    {
    case Playing:
        return ( mdate() - m_time_playback_started ) + m_ts_local_start;
#ifdef CHROMECAST_VERBOSE
    case Ready:
        msg_Dbg(m_module, "receiver idle using buffering time %" PRId64, m_ts_local_start);
        break;
    case Buffering:
        msg_Dbg(m_module, "receiver buffering using buffering time %" PRId64, m_ts_local_start);
        break;
    case Paused:
        msg_Dbg(m_module, "receiver paused using buffering time %" PRId64, m_ts_local_start);
        break;
#endif
    default:
        break;
    }
    return m_ts_local_start;
}

double intf_sys_t::getPlaybackPosition() const
{
    if( m_length > 0 )
        return (double) getPlaybackTimestamp() / (double)( m_length );
    return 0.0;
}

void intf_sys_t::setInitialTime(mtime_t time)
{
    if( time )
        m_ts_local_start = time;
}

void intf_sys_t::setState( States state )
{
    if ( m_state != state )
    {
#ifndef NDEBUG
        msg_Dbg( m_module, "Switching from state %s to %s", StateToStr( m_state ), StateToStr( state ) );
#endif
        m_state = state;
        vlc_cond_signal( &m_stateChangedCond );
    }
}

mtime_t intf_sys_t::get_time(void *pt)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    vlc_mutex_locker locker( &p_this->m_lock );
    return p_this->getPlaybackTimestamp();
}

double intf_sys_t::get_position(void *pt)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    vlc_mutex_locker locker( &p_this->m_lock );
    return p_this->getPlaybackPosition();
}

void intf_sys_t::set_initial_time(void *pt, mtime_t time )
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    vlc_mutex_locker locker( &p_this->m_lock );
    return p_this->setInitialTime( time );
}

void intf_sys_t::set_length(void *pt, mtime_t length)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    p_this->m_length = length;
}

void intf_sys_t::wait_app_started(void *pt)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    vlc_mutex_locker locker( &p_this->m_lock);
    p_this->waitAppStarted();
}

void intf_sys_t::request_seek(void *pt, mtime_t pos)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    p_this->requestPlayerSeek(pos);
}

void intf_sys_t::wait_seek_done(void *pt)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    p_this->waitSeekDone();
}

void intf_sys_t::set_pause_state(void *pt, bool paused)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    p_this->setPauseState( paused );
}

void intf_sys_t::set_meta(void *pt, vlc_meta_t *p_meta)
{
    intf_sys_t *p_this = static_cast<intf_sys_t*>(pt);
    p_this->setMeta( p_meta );
}
