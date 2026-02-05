/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus - netplay.c                                               *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2020 loganmc10                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#define SETTINGS_SIZE 24

#define M64P_CORE_PROTOTYPES 1
#include <enet/enet.h>
#include "api/callbacks.h"
#include "main.h"
#include "util.h"
#include "plugin/plugin.h"
#include "backends/plugins_compat/plugins_compat.h"
#include "netplay.h"
#include <string.h>

// Methods to streamline writing/reading ENet values to emulator
static inline void Net_Write32(uint32_t value, void *areap)
{
    uint32_t temp = ENET_HOST_TO_NET_32(value);
    memcpy(areap, &temp, 4);
}

static inline uint32_t Net_Read32(const void *areap)
{
    uint32_t temp;
    memcpy(&temp, areap, 4);
    return ENET_NET_TO_HOST_32(temp);
}

static inline void Net_Write16(uint16_t value, void *areap)
{
    uint16_t temp = ENET_HOST_TO_NET_16(value);
    memcpy(areap, &temp, 2);
}

static inline uint16_t Net_Read16(const void *areap)
{
    uint16_t temp;
    memcpy(&temp, areap, 2);
    return ENET_NET_TO_HOST_16(temp);
}

// Some of these are from legacy netplay and don't really do anything anymore but I added comments anyway

static int l_canFF; // Lag compensator, unused right now
static int l_netplay_controller;
static int l_netplay_control[4]; // The controllers being used. -1 if not present, otherwise their controller id (0-3)
static struct netplay_event* l_early_events[4]; // Buffer for packets received before registration
static ENetHost* l_host = NULL; // Player 1 (creates the room)
static ENetPeer* l_peer = NULL; // Player 2 (joins the room)
static int l_spectator; // Unused for now
static int l_netplay_is_init = 0; // If set back to zero, main loop will shut down netplay
static uint32_t l_vi_counter; // Counts VI interrupts as baseline frames
static uint8_t l_status; // Connected, desynced, disconnectd
static uint32_t l_reg_id; // Each player gets a registration ID in their instance
static struct controller_input_compat *l_cin_compats; // Controller structures that the rest of the emulator reads
static uint8_t l_plugin[4]; // Controller paks, hard coded to none for everybody right now
static uint8_t l_buffer_target; // Delays inputs for syncing
static uint8_t l_player_lag[4];
static uint32_t l_last_inputs[4]; // Keeps track of players' last inputs. If a new input is invalid then it uses this.

static uint32_t l_last_send_vi[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu }; // The last VI on which a player sent input
static uint32_t l_cached_vi[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu }; // Cache for prior VI
static uint32_t l_cached_inputs[4] = { 0u, 0u, 0u, 0u };

static uint32_t l_sync_vi = 0xffffffffu; // The last VI on which a sync happened
static uint32_t l_sync_regs[CP0_REGS_COUNT];

static uint8_t* l_incoming_data = NULL;
static size_t l_incoming_size = 0;
static int l_is_host = 0; // Whether the user is the one who created the room (Player 1)
static int l_client_ready = 0; // If above is true, whether the other player is ready

static const int32_t l_check_sync_packet_size = (CP0_REGS_COUNT * 4) + 5;

// Packet formats
#define PACKET_SEND_KEY_INFO 0
#define PACKET_RECEIVE_KEY_INFO 1
#define PACKET_REQUEST_KEY_INFO 2
#define PACKET_RECEIVE_KEY_INFO_GRATUITOUS 3
#define PACKET_SYNC_DATA 4
#define PACKET_SEND_SAVE 10
#define PACKET_RECEIVE_SAVE 11
#define PACKET_SEND_SETTINGS 12
#define PACKET_RECEIVE_SETTINGS 13
#define PACKET_REGISTER_PLAYER 14
#define PACKET_GET_REGISTRATION 15
#define PACKET_RECEIVE_REGISTRATION 16
#define PACKET_CLIENT_READY 17

// Relay Protocol
#define NRLY_MAGIC_BE 0x4E524C59u // Packet identifier 'N' 'R' 'L' 'Y' (Netplay ReLaY teehee)
#define NRLY_VERSION  1u

// Types of messages for ENet
typedef enum nrly_msg_type_t
{
    NRLY_MSG_HELLO = 0x01,
    NRLY_MSG_READY = 0x02,
    NRLY_MSG_ERROR = 0x03,
    NRLY_MSG_DATA_BIND = 0x10,
} nrly_msg_type_t;

// Failure types that ENet can give
typedef enum nrly_error_code_t
{
    NRLY_ERR_INVALID_TOKEN      = 0x01,
    NRLY_ERR_TOKEN_EXPIRED      = 0x02,
    NRLY_ERR_ROLE_ALREADY_TAKEN = 0x03,
    NRLY_ERR_UNKNOWN_ROOM       = 0x04,
    NRLY_ERR_MALFORMED          = 0x05,
    NRLY_ERR_RATE_LIMITED       = 0x06,
} nrly_error_code_t;

// Input buffer, should experiment with the value
#define INPUT_BUF 256
typedef struct {
  uint32_t count;
  uint32_t inputs;
  uint8_t plugin;
  uint8_t valid;
} input_slot;

// Circular buffer for inputs, should be more efficient for FIFO
input_slot l_input_ring[4][INPUT_BUF];

#define RELAY_DATA_PORT 27015 // Server port for relaying packets
#define RELAY_CTRL_PORT 27016 // Server port for establishing connection

#define NETPLAY_DEFAULT_INPUT_DELAY 6

// Disconnect helper
static void disconnect_and_cleanup(void);

// Make contact with relay server and submit data
static int relay_ctrl_handshake(const char* relay_host, uint16_t ctrl_port,
                                const char* token, uint16_t local_data_port);
static int relay_data_bind(ENetSocket enet_sock, const char* relay_host,
                                uint16_t data_port, const char* token);


m64p_error netplay_start(const char* relay_host, const char* token, int is_host)
{
    // Host address and token must be given in command parameter. Emulator should never get to this point but who knows
    if (!relay_host || relay_host[0] == '\0' || !token || token[0] == '\0')
    {
        DebugMessage(M64MSG_ERROR, "Netplay: Missing relay host or token!");
        return M64ERR_INPUT_INVALID;
    }

    l_is_host = (is_host == 1);

    if (enet_initialize() != 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: ENet init failed.");
        return M64ERR_SYSTEM_FAIL;
    }

    // Bind ENet to a local port so we know what to report in HELLO
    ENetAddress local = { 0 };
    local.host = ENET_HOST_ANY;
    local.port = 0;

    l_host = enet_host_create(&local, 1, 2, 0, 0);
    if (!l_host)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: Failed to create ENet host.");
        enet_deinitialize();
        return M64ERR_SYSTEM_FAIL;
    }

    uint16_t local_port = l_host->address.port;
    if (local_port == 0)
    {
        ENetAddress bound;
        if (enet_socket_get_address(l_host->socket, &bound) == 0)
            local_port = bound.port;
    }
    DebugMessage(M64MSG_INFO, "Netplay: relay_host='%s' token_len=%u is_host=%d",
    relay_host, (unsigned)strlen(token), l_is_host);

    DebugMessage(M64MSG_INFO, "Netplay: Local ENet port=%u. Sending relay HELLO...", (unsigned)local_port);

    // CONTROL handshake on 27016
    if (!relay_ctrl_handshake(relay_host, RELAY_CTRL_PORT, token, local_port))
    {
        DebugMessage(M64MSG_ERROR, "Netplay: Relay CONTROL handshake failed.");
        enet_host_destroy(l_host);
        enet_deinitialize();
        l_host = NULL;
        return M64ERR_SYSTEM_FAIL;
    }

    if (!relay_data_bind(l_host->socket, relay_host, RELAY_DATA_PORT, token))
    {
        DebugMessage(M64MSG_ERROR, "Netplay: Relay DATA_BIND failed.");
        enet_host_destroy(l_host);
        enet_deinitialize();
        l_host = NULL;
        return M64ERR_SYSTEM_FAIL;
    }

    // ENet connection to relay data port 27015
    ENetAddress relay_addr = { 0 };
    if (enet_address_set_host(&relay_addr, relay_host) != 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: Failed to resolve relay host %s", relay_host);
        enet_host_destroy(l_host);
        enet_deinitialize();
        l_host = NULL;
        return M64ERR_SYSTEM_FAIL;
    }
    relay_addr.port = RELAY_DATA_PORT;

    // If you're the host, wait for client to connect and set them to l_peer
    if (l_is_host)
    {
        ENetEvent event;
        int ok = 0;
        uint32_t start = SDL_GetTicks();
        
        SDL_Delay(100);
    
        while ((SDL_GetTicks() - start) < 15000)
        {
            int r = enet_host_service(l_host, &event, 100);
            if (r > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            {
                l_peer = event.peer;
                ok = 1;
                break;
            }
        }
    
        if (!ok)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: ENet connect timeout.");
                
            enet_host_destroy(l_host);
            enet_deinitialize();
            l_host = NULL;
            return M64ERR_SYSTEM_FAIL;
        }
    }
    // Otherwise, l_peer is the host and you connect to them
    else
    {
        l_peer = enet_host_connect(l_host, &relay_addr, 2, 0);
        if (!l_peer)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: Failed to connect to relay data port.");
            enet_host_destroy(l_host);
            enet_deinitialize();
            l_host = NULL;
            return M64ERR_SYSTEM_FAIL;
        }
    
        ENetEvent event;
        int ok = 0;
        int rejected = 0;
        uint32_t start = SDL_GetTicks();
        
        SDL_Delay(100);
    
        while ((SDL_GetTicks() - start) < 15000)
        {
            int r = enet_host_service(l_host, &event, 100);
            if (r > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
            {
                ok = 1;
                break;
            }
            if (r > 0 && event.type == ENET_EVENT_TYPE_DISCONNECT)
            {
                rejected = 1; // Mostly here for debugging, tells whether client timed out or was actively rejected
                break;
            }
        }
    
        if (!ok)
        {
            if (rejected)
                DebugMessage(M64MSG_ERROR, "Netplay: Connection rejected by Relay.");
            else
                DebugMessage(M64MSG_ERROR, "Netplay: ENet connect timeout.");
                
            disconnect_and_cleanup();
            return M64ERR_SYSTEM_FAIL;
        }
    }

    // Initialize netplay state
    for (int i = 0; i < 4; ++i)
    {
        l_netplay_control[i] = -1;
        l_plugin[i] = 0;
        l_player_lag[i] = 0;
        l_last_inputs[i] = 0;
        l_early_events[i] = NULL;
        for (int j = 0; j < INPUT_BUF; ++j) l_input_ring[i][j].valid = 0;
    }

    l_canFF = 0;
    l_netplay_controller = 0;
    l_netplay_is_init = 1;
    l_spectator = 1;
    l_vi_counter = 0;
    l_status = 0;
    l_reg_id = 0;
    l_buffer_target = NETPLAY_DEFAULT_INPUT_DELAY;

    if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
    l_incoming_size = 0;

    DebugMessage(M64MSG_INFO, "Netplay: connected. is_host=%d", l_is_host);
    return M64ERR_SUCCESS;
}


m64p_error netplay_stop()
{
    if (!l_host)
        return M64ERR_INVALID_STATE;
    else
    {
        if (l_cin_compats != NULL)
        {
            for (int i = 0; i < 4; ++i)
            {
                struct netplay_event* current = l_cin_compats[i].event_first;
                struct netplay_event* next;
                while (current != NULL)
                {
                    next = current->next;
                    free(current);
                    current = next;
                }
            }
        }
    }
    
    // Clean up early buffer
    for (int i = 0; i < 4; ++i)
    {
        struct netplay_event* current = l_early_events[i];
        struct netplay_event* next;
        while (current != NULL)
        {
            next = current->next;
            free(current);
            current = next;
        }
        l_early_events[i] = NULL;
    }
    disconnect_and_cleanup();

    return M64ERR_SUCCESS;
}

static void disconnect_and_cleanup(void)
{
    if (l_peer != NULL)
    {
        enet_peer_disconnect(l_peer, 0);
        
        // Allow up to 3 seconds for the disconnect to succeed
        ENetEvent event;
        while (enet_host_service(l_host, &event, 3000) > 0)
        {
            switch (event.type)
            {
                case ENET_EVENT_TYPE_RECEIVE:
                    enet_packet_destroy(event.packet);
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    goto disconnect_success;
                default:
                    break;
            }
        }
        
        // Force reset if not disconnected within timeout
        enet_peer_reset(l_peer);
    }
    
disconnect_success:
    enet_host_destroy(l_host);
    enet_deinitialize();
    
    l_host = NULL;
    l_peer = NULL;
    l_is_host = 0;
    l_netplay_is_init = 0;
    
    if (l_incoming_data)
        free(l_incoming_data);
    l_incoming_data = NULL;
}

static uint8_t buffer_size(uint8_t control_id)
{
    uint8_t counter = 0;
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current != NULL)
    {
        current = current->next;
        ++counter;
    }
    return counter;
}

// Check if we already have this input stored
static int check_valid(uint8_t control_id, uint32_t count)
{
    int idx = count % INPUT_BUF;
    if (l_input_ring[control_id][idx].valid && l_input_ring[control_id][idx].count == count)
        return 1;
    return 0;
}

static void netplay_poll(void)
{
    ENetEvent event;
    
    // Only poll if there isn't any pending data waiting to be consumed
    while (enet_host_service(l_host, &event, 0) > 0)
    {
        switch (event.type)
        {
            case ENET_EVENT_TYPE_CONNECT:
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                DebugMessage(M64MSG_ERROR, "Netplay: Disconnected from server.");
                disconnect_and_cleanup();
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                // Handle packet
                {
                    // DebugMessage(M64MSG_INFO, "Netplay: channel ID: %u, packet len: %u",
                    //              (unsigned)event.channelID,
                    //              (unsigned)event.packet->dataLength);
                    uint8_t* data = event.packet->data;
                    size_t len = event.packet->dataLength;
                    
                    if (len < 1) { enet_packet_destroy(event.packet); continue; }

                    // Match the packet type
                    switch (data[0])
                    {
                        case PACKET_SYNC_DATA:
                            if (len < l_check_sync_packet_size) break;

                            uint32_t sync_vi = Net_Read32(&data[1]);
                            
                            if (sync_vi == l_sync_vi)
                            {
                                int error = 0;
                                for (int i = 0; i < CP0_REGS_COUNT; ++i)
                                {
                                    uint32_t val = Net_Read32(&data[(i * 4) + 5]);
                                    if (val != l_sync_regs[i])
                                    {
                                        DebugMessage(M64MSG_ERROR, "Netplay: Sync Error at VI %u. Reg %d: Local %X Remote %X", (unsigned)sync_vi, i, l_sync_regs[i], val);
                                        error = 1;
                                    }
                                }

                                if (error)
                                {
                                    DebugMessage(M64MSG_ERROR, "Netplay: Synchronization failure detected.");
                                }
                            }
                            break;
                        case PACKET_REGISTER_PLAYER: // Server side handler
                            if (l_is_host)
                            {
                                if (len < 8) break;
                                // Client registering. ACK with [player_id, buffer_target]
                                uint8_t player_id = data[1];
                                uint8_t resp[2] = { player_id, (uint8_t)l_buffer_target };
                                ENetPacket* p = enet_packet_create(resp, 2, ENET_PACKET_FLAG_RELIABLE);
                                enet_peer_send(event.peer, 0, p);
                            }
                            break;
                        case PACKET_CLIENT_READY:
                            if (l_is_host)
                            {
                                // DebugMessage(M64MSG_INFO, "Netplay: Client Ready");
                                l_client_ready = 1;
                            }
                            break;
                        case PACKET_GET_REGISTRATION: // Host side handler
                            if (l_is_host)
                            {
                                // DebugMessage(M64MSG_INFO, "Netplay: Client requested registration");
                                // Send 24 bytes of registration info wrapped in packet
                                uint8_t resp[25] = {0};
                                resp[0] = PACKET_RECEIVE_REGISTRATION;
                                
                                uint32_t curr = 1;
                                // P1 (Host)
                                Net_Write32(1, &resp[curr]); curr+=4; // ID!=0 means present
                                resp[curr++] = PLUGIN_NONE; resp[curr++] = 0;
                                // P2 (Client)
                                Net_Write32(2, &resp[curr]); curr+=4;
                                resp[curr++] = PLUGIN_NONE; resp[curr++] = 0;
                                // P3
                                Net_Write32(0, &resp[curr]); curr+=4; // Not present
                                resp[curr++] = PLUGIN_NONE; resp[curr++] = 0;
                                // P4
                                Net_Write32(0, &resp[curr]); curr+=4;
                                resp[curr++] = PLUGIN_NONE; resp[curr++] = 0;
                                
                                ENetPacket* p = enet_packet_create(resp, 25, ENET_PACKET_FLAG_RELIABLE);
                                enet_peer_send(event.peer, 0, p);
                            }
                            break;
                        case PACKET_SEND_KEY_INFO: // Host side handler
                            if (l_is_host)
                            {
                                if (len < 11)
                                    break;
                                // Client sent input.
                                uint8_t player = data[1];
                                uint32_t count = Net_Read32(&data[2]);
                                uint32_t inputs = Net_Read32(&data[6]);
                                uint8_t plugin = data[10];

                                if (player > 3)
                                    break;

                                if (l_cin_compats == NULL) 
                                    break;

                                // Store all inputs locally
                                struct controller_input_compat* pComp = &l_cin_compats[player];
                                // Drop inputs if they go too long without being consumed
                                if (count + 240u < l_vi_counter)
                                {
                                    DebugMessage(M64MSG_WARNING, "Netplay: Dropping P%u input for count %u (too old, VI %u)", player+1, count, l_vi_counter);
                                    break;
                                }
                                if (!check_valid(player, count))
                                {
                                    int idx = count % INPUT_BUF;
                                    l_input_ring[player][idx].count = count;
                                    l_input_ring[player][idx].inputs = inputs;
                                    l_input_ring[player][idx].plugin = plugin;
                                    l_input_ring[player][idx].valid = 1;
                                }
                            }
                            break;
                            
                        case PACKET_RECEIVE_KEY_INFO: // Client side handler
                        {
                            if (len < 6) break;
                            uint8_t player = data[1];
                            uint32_t count_dbg = Net_Read32(&data[5]);
                            
                            uint8_t current_status = data[2];
                            uint8_t lag = data[3];
                            uint8_t count_events = data[4];

                            if (player > 3) break;
                            
                            l_player_lag[player] = lag;
                            
                            if (l_cin_compats != NULL && current_status != l_status)
                            {
                                int prev_sync = (l_status & 1);
                                int curr_sync = (current_status & 1);
                                if (prev_sync != curr_sync)
                                {
                                    DebugMessage(M64MSG_ERROR, "Netplay: players have de-synced at VI %u", l_vi_counter);
                                }
                                
                                for (int dis = 1; dis < 5; ++dis)
                                {
                                    int prev_sync = (l_status & (1 << dis));
                                    int curr_sync = (l_status & (1 << dis));
                                    if (prev_sync != curr_sync)
                                    {
                                        DebugMessage(M64MSG_ERROR, "Netplay: player %u has disconnected", dis);
                                        disconnect_and_cleanup();
                                    }
                                }
                                l_status = current_status;
                            }
                            
                            size_t curr = 5;
                            for (int i = 0; i < count_events; ++i)
                            {
                                if (curr + 9 > len) break;
                                
                                uint32_t count = Net_Read32(&data[curr]);
                                curr += 4;
                                uint32_t inputs = Net_Read32(&data[curr]);
                                curr += 4;
                                uint8_t plugin = data[curr];
                                curr += 1;
                                
                                int idx = count % INPUT_BUF;
                                l_input_ring[player][idx].count = count;
                                l_input_ring[player][idx].inputs = inputs;
                                l_input_ring[player][idx].plugin = plugin;
                                l_input_ring[player][idx].valid = 1;
                            }
                            break;
                        }
                        default:
                            if (l_incoming_data == NULL)
                            {
                                l_incoming_data = malloc(len);
                                memcpy(l_incoming_data, data, len);
                                l_incoming_size = len;
                                enet_packet_destroy(event.packet);
                                return;
                            }
                            else
                            {
                                free(l_incoming_data);
                                l_incoming_data = malloc(len);
                                memcpy(l_incoming_data, data, len);
                                l_incoming_size = len;
                                enet_packet_destroy(event.packet);
                                return;
                            }
                            break;
                    }
                    enet_packet_destroy(event.packet);
                }
                break;
        }
    }
}

// Unused rn
static void netplay_delete_event(struct netplay_event* current, uint8_t control_id)
{
    struct netplay_event* find = l_cin_compats[control_id].event_first;
    while (find != NULL)
    {
        if (find->next == current)
        {
            find->next = current->next;
            break;
        }
        find = find->next;
    }
    if (current == l_cin_compats[control_id].event_first)
        l_cin_compats[control_id].event_first = l_cin_compats[control_id].event_first->next;
    free(current);
}


static uint32_t netplay_get_input_for_vi(uint8_t control_id, uint32_t vi)
{
    uint32_t inputs = 0;
    
    // Process incoming packets
    netplay_poll();

    main_core_state_set(M64CORE_SPEED_LIMITER, 1);
    l_canFF = 0;
    
    uint32_t start_wait = SDL_GetTicks();
    while (!check_valid(control_id, vi) && (SDL_GetTicks() - start_wait) < 500)
    {
        netplay_poll();
        if (check_valid(control_id, vi))
            break;
        SDL_Delay(1);
    }

    if (check_valid(control_id, vi))
    {
        int idx = vi % INPUT_BUF;
        inputs = l_input_ring[control_id][idx].inputs;
        Controls[control_id].Plugin = l_input_ring[control_id][idx].plugin;
        l_last_inputs[control_id] = inputs;
    }
    else
    {
        inputs = l_last_inputs[control_id];
    }
    
    return inputs;
}

// Store local input into ring buffer
static void netplay_insert_local_event(uint8_t control_id, uint32_t vi, uint32_t keys)
{
    int idx = vi % INPUT_BUF;
    l_input_ring[control_id][idx].count = vi;
    l_input_ring[control_id][idx].inputs = keys;
    l_input_ring[control_id][idx].plugin = l_plugin[control_id];
    l_input_ring[control_id][idx].valid = 1;
}

// Send local input to peer
static void netplay_send_scheduled_input(uint8_t control_id, uint32_t vi, uint32_t keys)
{
    if (l_is_host)
    {
        uint8_t pkt[14];
        pkt[0] = PACKET_RECEIVE_KEY_INFO;
        pkt[1] = control_id;
        pkt[2] = l_status;
        pkt[3] = 0;
        pkt[4] = 1;
        Net_Write32(vi, &pkt[5]);
        Net_Write32(keys, &pkt[9]);
        pkt[13] = l_plugin[control_id];

        ENetPacket* p = enet_packet_create(pkt, 14, 0);
        enet_peer_send(l_peer, 0, p);
    }
    else
    {
        uint8_t pkt[11];
        pkt[0] = PACKET_SEND_KEY_INFO;
        pkt[1] = control_id;
        Net_Write32(vi, &pkt[2]);
        Net_Write32(keys, &pkt[6]);
        pkt[10] = l_plugin[control_id];

        ENetPacket* p = enet_packet_create(pkt, 11, 0);
        enet_peer_send(l_peer, 1, p);
    }
}

uint8_t netplay_register_player(uint8_t player, uint8_t plugin, uint8_t rawdata, uint32_t reg_id)
{
    l_reg_id = reg_id;
    uint8_t data[8];
    data[0] = PACKET_REGISTER_PLAYER;
    data[1] = player; 
    data[2] = plugin; 
    data[3] = rawdata; 
    Net_Write32(l_reg_id, &data[4]);

    // Send reliable on Channel 0 (Control)
    ENetPacket* packet = enet_packet_create(data, 8, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(l_peer, 0, packet);
    enet_host_flush(l_host);

    // Wait for response
    uint8_t response[2] = {0, 0};
    uint32_t start = SDL_GetTicks();
    
    // Clear temp buffer
    if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
    
    while ((SDL_GetTicks() - start) < 5000)
    {
        netplay_poll();
        if (l_incoming_data && l_incoming_size >= 2)
        {
             memcpy(response, l_incoming_data, 2);
             free(l_incoming_data); l_incoming_data = NULL;
             break;
        }
        SDL_Delay(10);
    }
    
    l_buffer_target = response[1]; 
    return response[0]; 
}

int netplay_is_init()
{
    return l_netplay_is_init;
}

int netplay_lag()
{
    return l_canFF;
}

int netplay_next_controller()
{
    return l_netplay_controller;
}

void netplay_set_controller(uint8_t player)
{
    l_netplay_control[player] = l_netplay_controller++;
    l_spectator = 0;
}

int netplay_get_controller(uint8_t player)
{
    return l_netplay_control[player];
}

file_status_t netplay_read_storage(const char *filename, void *data, size_t size)
{
    // Syncs save games, might be useless if we're just loading a state from S3 anyway
    const char *file_extension = strrchr(filename, '.');
    file_extension += 1;

    uint32_t buffer_pos = 0;
    char *output_data = malloc(size + strlen(file_extension) + 6);
    
    ENetPacket* packet;

    file_status_t ret;
    uint8_t request;

    if (l_is_host) 
    {
        if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
        
        uint32_t start = SDL_GetTicks();
        int got_request = 0;
        
        while ((SDL_GetTicks() - start) < 30000)
        {
            netplay_poll();
            if (l_incoming_data)
            {
                if (l_incoming_data[0] == PACKET_RECEIVE_SAVE)
                {
                    got_request = 1;
                    free(l_incoming_data); l_incoming_data = NULL;
                    break;
                }
                else
                {
                    free(l_incoming_data); l_incoming_data = NULL;
                }
            }
            SDL_Delay(1);
        }

        if (!got_request)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: Timeout waiting for client save request %s", filename);
            free(output_data);
            return file_open_error; // Continue but failed sync
        }

        // Send Data
        request = PACKET_SEND_SAVE;
        memcpy(&output_data[buffer_pos], &request, 1);
        ++buffer_pos;

         //send file extension
        memcpy(&output_data[buffer_pos], file_extension, strlen(file_extension) + 1);
        buffer_pos += strlen(file_extension) + 1;

        ret = read_from_file(filename, data, size);
        if (ret == file_open_error)
            memset(data, 0, size); //all zeros means there is no save file
        Net_Write32((int32_t)size, &output_data[buffer_pos]); //file data size
        buffer_pos += 4;
        memcpy(&output_data[buffer_pos], data, size); //file data
        buffer_pos += size;

        packet = enet_packet_create(output_data, buffer_pos, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(l_peer, 0, packet);
        enet_host_flush(l_host);
    }
    else
    {
        // Client: Send Request
        request = PACKET_RECEIVE_SAVE;
        memcpy(&output_data[buffer_pos], &request, 1);
        ++buffer_pos;

        // Extension of the file we are requesting
        memcpy(&output_data[buffer_pos], file_extension, strlen(file_extension) + 1);
        buffer_pos += strlen(file_extension) + 1;

        packet = enet_packet_create(output_data, buffer_pos, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(l_peer, 0, packet);
        enet_host_flush(l_host);
        
        // Wait for response
        if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
        
        memset(data, 0, size); // Clear dest
        ret = file_open_error;
        
        uint32_t start = SDL_GetTicks();
        while ((SDL_GetTicks() - start) < 30000) // 30s timeout for large saves
        {
            netplay_poll();
            if (l_incoming_data)
            {
                if (l_incoming_data[0] == PACKET_SEND_SAVE && l_incoming_size > 5)
                {
                    // Parse packet: [ID 1] [Ext string 1..N] [\0 1] [Size 4] [Data ...]
                    size_t curr = 1;
                    // Skip extension string
                    while (curr < l_incoming_size && l_incoming_data[curr] != 0) curr++;
                    curr++; // Skip null terminator
                    
                    if (curr + 4 <= l_incoming_size)
                    {
                        uint32_t data_size = Net_Read32(&l_incoming_data[curr]);
                        curr += 4;
                        if (data_size == size && curr + data_size <= l_incoming_size)
                        {
                            memcpy(data, &l_incoming_data[curr], size);
                            
                            char *data_array = data;
                            int sum = 0;
                            for (int i = 0; i < size; ++i)
                                sum |= data_array[i];

                            if (sum == 0) //all zeros means there is no save file
                                ret = file_open_error;
                            else
                                ret = file_ok;
                        }
                    }
                    
                    free(l_incoming_data); l_incoming_data = NULL;
                    break;
                }
                else
                {
                    free(l_incoming_data); l_incoming_data = NULL;
                }
            }
            SDL_Delay(10);
        }
    }
    free(output_data);
    return ret;
}

void netplay_sync_settings(uint32_t *count_per_op, uint32_t *count_per_op_denom_pot, uint32_t *disable_extra_mem, int32_t *si_dma_duration, uint32_t *emumode, int32_t *no_compiled_jump)
{
    if (!netplay_is_init())
        return;

    char output_data[SETTINGS_SIZE + 1];
    uint8_t request;
    
    // Host sends settings, client receives
    if (l_is_host) 
    {
        request = PACKET_SEND_SETTINGS;
        memcpy(&output_data[0], &request, 1);
        Net_Write32(*count_per_op, &output_data[1]);
        Net_Write32(*count_per_op_denom_pot, &output_data[5]);
        Net_Write32(*disable_extra_mem, &output_data[9]);
        Net_Write32(*si_dma_duration, &output_data[13]);
        Net_Write32(*emumode, &output_data[17]);
        Net_Write32(*no_compiled_jump, &output_data[21]);
        
        ENetPacket* packet = enet_packet_create(output_data, SETTINGS_SIZE + 1, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(l_peer, 0, packet);
        enet_host_flush(l_host);
    }
    else
    {
        if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
        
        uint32_t start = SDL_GetTicks();
        while ((SDL_GetTicks() - start) < 5000)
        {
            netplay_poll();
            if (l_incoming_data)
            {
                if (l_incoming_data[0] == PACKET_SEND_SETTINGS && l_incoming_size >= SETTINGS_SIZE + 1)
                {
                    memcpy(output_data, l_incoming_data + 1, SETTINGS_SIZE); 
                    free(l_incoming_data); l_incoming_data = NULL;
                    
                    *count_per_op = Net_Read32(&output_data[0]);
                    *count_per_op_denom_pot = Net_Read32(&output_data[4]);
                    *disable_extra_mem = Net_Read32(&output_data[8]);
                    *si_dma_duration = Net_Read32(&output_data[12]);
                    *emumode = Net_Read32(&output_data[16]);
                    *no_compiled_jump = Net_Read32(&output_data[20]);
                    return;
                }
                else
                {
                    free(l_incoming_data); l_incoming_data = NULL;
                }
            }
            SDL_Delay(10);
        }
    }
}

void netplay_check_sync(struct cp0* cp0)
{
    if (!netplay_is_init())
        return;

    // Check sync every 600 frames
    if (l_vi_counter % 600 == 0)
    {
        const uint32_t* cp0_regs = r4300_cp0_regs(cp0);

        l_sync_vi = l_vi_counter;
        for (int i = 0; i < CP0_REGS_COUNT; ++i)
        {
            l_sync_regs[i] = cp0_regs[i];
        }

        uint8_t data[ (CP0_REGS_COUNT * 4) + 5 ];

        data[0] = PACKET_SYNC_DATA;
        Net_Write32(l_vi_counter, &data[1]); //current VI count
        for (int i = 0; i < CP0_REGS_COUNT; ++i)
        {
            Net_Write32(cp0_regs[i], &data[(i * 4) + 5]);
        }
        
        ENetPacket* packet = enet_packet_create(data, (CP0_REGS_COUNT * 4) + 5, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(l_peer, 0, packet);
    }

    ++l_vi_counter;
}

static void netplay_flush_early_buffer(void)
{
    if (!l_cin_compats) return;

    for (int i = 0; i < 4; ++i)
    {
        if (l_early_events[i])
        {
            struct netplay_event* ev = l_early_events[i];
            while (ev)
            {
                struct netplay_event* next = ev->next;
                ev->next = l_cin_compats[i].event_first;
                l_cin_compats[i].event_first = ev;
                
                ev = next;
            }
            l_early_events[i] = NULL;
        }
    }
}

void netplay_read_registration(struct controller_input_compat* cin_compats)
{
    //This function runs right before the game starts
    //The server shares the registration details about each player
    if (!netplay_is_init())
        return;
    
    l_cin_compats = cin_compats;
    
    // Flush any buffered inputs
    netplay_flush_early_buffer();

    if (l_is_host)
    {
        // Force all paks to none for now
        // P1 (Host)
        Controls[0].Present = 1;
        Controls[0].Plugin = PLUGIN_NONE;
        Controls[0].RawData = 0;
        l_plugin[0] = PLUGIN_NONE;
        netplay_set_controller(0);

        // P2 (Client)
        Controls[1].Present = 1;
        Controls[1].Plugin = PLUGIN_NONE;
        Controls[1].RawData = 0;
        l_plugin[1] = PLUGIN_NONE;
        
        // Host waits for client to be ready
        uint32_t start = SDL_GetTicks();
        while ((SDL_GetTicks() - start) < 30000) 
        {
            // Clear unused incoming data to prevent deadlock in netplay_poll
            if (l_incoming_data)
            {
            free(l_incoming_data); l_incoming_data = NULL;
            }

            netplay_poll();
            if (l_client_ready) break;
            SDL_Delay(10);
        }
        if (!l_client_ready) DebugMessage(M64MSG_ERROR, "Netplay: Timed out waiting for client ready.");
        
        return;
    }

    // P3/P4 (Shouldn't be present)
    Controls[2].Present = 0;
    Controls[2].Plugin = PLUGIN_NONE;
    Controls[3].Present = 0;
    Controls[3].Plugin = PLUGIN_NONE;

    uint32_t reg_id;
    char output_data = PACKET_GET_REGISTRATION;
    char input_data[24];
    
    ENetPacket* packet = enet_packet_create(&output_data, 1, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(l_peer, 0, packet);
    enet_host_flush(l_host);
    
    // Wait for response
    if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
    
    uint32_t start = SDL_GetTicks();
    while ((SDL_GetTicks() - start) < 10000) 
    {
        netplay_poll();
        if (l_incoming_data && 
            l_incoming_data[0] == PACKET_RECEIVE_REGISTRATION && 
            l_incoming_size >= 25)
        {
            memcpy(input_data, &l_incoming_data[1], 24);
            free(l_incoming_data); l_incoming_data = NULL;
            break;
        }
        else if (l_incoming_data)
        {
             free(l_incoming_data); l_incoming_data = NULL;
        }
        SDL_Delay(10);
    }
    
    uint32_t curr = 0;
    for (int i = 0; i < 4; ++i)
    {
        reg_id = Net_Read32(&input_data[curr]);
        curr += 4;

        Controls[i].Type = CONT_TYPE_STANDARD; //make sure VRU is disabled

        if (reg_id == 0)
        {
            Controls[i].Present = 0;
            Controls[i].Plugin = PLUGIN_NONE;
            Controls[i].RawData = 0;
            curr += 2;
        }
        else
        {
            Controls[i].Present = 1;
            if (i > 0 && input_data[curr] == PLUGIN_MEMPAK)
                Controls[i].Plugin = PLUGIN_NONE;
            else if (input_data[curr] == PLUGIN_TRANSFER_PAK)
                Controls[i].Plugin = PLUGIN_NONE;
            else
                Controls[i].Plugin = input_data[curr];
            l_plugin[i] = Controls[i].Plugin;
            ++curr;
            Controls[i].RawData = input_data[curr];
            ++curr;
            
            if (i == 1)
            {
               netplay_set_controller(1);
            }
        }
    }
    
    // for (int i=0; i<4; ++i)
    // {
    //     if (Controls[i].Present == 0)
    //     {
    //          Controls[i].Plugin = PLUGIN_NONE;
    //     }
    // }
    
    // Send Ready Signal
    output_data = PACKET_CLIENT_READY;
    packet = enet_packet_create(&output_data, 1, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(l_peer, 0, packet);
    enet_host_flush(l_host);
    
    // DebugMessage(M64MSG_INFO, "Netplay: Client registration complete. Sending Ready.");
}

static void netplay_send_raw_input(struct pif* pif)
{
    uint32_t vi = l_vi_counter;
    for (int i = 0; i < 4; ++i)
    {
        if (l_netplay_control[i] != -1)
        {
            if (pif->channels[i].tx && pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
            {
                if (l_last_send_vi[i] == vi) // Already sent for this VI, continue
                    continue;
                l_last_send_vi[i] = vi;

                uint32_t keys_now = *(uint32_t*)pif->channels[i].rx_buf;
                uint32_t target_vi = vi + (uint32_t)l_buffer_target;

                netplay_insert_local_event((uint8_t)i, target_vi, keys_now);

                netplay_send_scheduled_input((uint8_t)i, target_vi, keys_now);
            }
        }
    }
}

static void netplay_get_raw_input(struct pif* pif)
{
    uint32_t vi = l_vi_counter;
    for (int i = 0; i < 4; ++i)
    {
        if (Controls[i].Present == 1)
        {
            if (pif->channels[i].tx)
            {
                *pif->channels[i].rx &= ~0xC0; //Always show the controller as connected

                if(pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
                {
                    if (l_cached_vi[i] != vi)
                    {
                        if (vi < (uint32_t)l_buffer_target)
                        {
                            l_cached_inputs[i] = 0;
                        }
                        else
                        {
                            l_cached_inputs[i] = netplay_get_input_for_vi((uint8_t)i, vi);
                        }
                        l_cached_vi[i] = vi;
                    }
                    *(uint32_t*)pif->channels[i].rx_buf = l_cached_inputs[i];
                }
                else if ((pif->channels[i].tx_buf[0] == JCMD_STATUS || pif->channels[i].tx_buf[0] == JCMD_RESET) && Controls[i].RawData)
                {
                    //a bit of a hack for raw input controllers, force the status
                    uint16_t type = JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT;
                    pif->channels[i].rx_buf[0] = (uint8_t)(type >> 0);
                    pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                    pif->channels[i].rx_buf[2] = 0;
                }
                else if (pif->channels[i].tx_buf[0] == JCMD_PAK_READ && Controls[i].RawData)
                {
                    //also a hack for raw input, we return "mempak not present" if the game tries to read the mempak
                    pif->channels[i].rx_buf[32] = 255;
                }
                else if (pif->channels[i].tx_buf[0] == JCMD_PAK_WRITE && Controls[i].RawData)
                {
                    //also a hack for raw input, we return "mempak not present" if the game tries to write to mempak
                    pif->channels[i].rx_buf[0] = 255;
                }
            }
        }
    }
}

void netplay_update_input(struct pif* pif)
{
    if (netplay_is_init())
    {
        netplay_send_raw_input(pif);
        netplay_get_raw_input(pif);
    }
}

m64p_error netplay_send_config(char* data, int size)
{
    if (!netplay_is_init())
        return M64ERR_NOT_INIT;

    if (l_netplay_control[0] != -1 || size == 1) //Only P1 sends settings, we allow all players to send if the size is 1, this may be a request packet
    {
        ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(l_peer, 0, packet);
        enet_host_flush(l_host);
        return M64ERR_SUCCESS;
    }
    else
        return M64ERR_INVALID_STATE;
}

m64p_error netplay_receive_config(char* data, int size)
{
    if (!netplay_is_init())
        return M64ERR_NOT_INIT;

    if (l_netplay_control[0] == -1) //Only P2-4 receive settings
    {
        // Wait for response
        if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
        
        uint32_t start = SDL_GetTicks();
        while ((SDL_GetTicks() - start) < 10000) // 10s timeout
        {
            netplay_poll();
            if (l_incoming_data && l_incoming_size >= size)
            {
                memcpy(data, l_incoming_data, size);
                free(l_incoming_data); l_incoming_data = NULL;
                return M64ERR_SUCCESS;
            }
            SDL_Delay(10);
        }
        return M64ERR_SYSTEM_FAIL;
    }
    else
        return M64ERR_INVALID_STATE;
}

// Host makes contact with rendezvous server
static int relay_ctrl_handshake(const char* relay_host, uint16_t ctrl_port,
                                const char* token, uint16_t local_data_port)
{
    ENetAddress relay_addr = {0};

    if (enet_address_set_host(&relay_addr, relay_host) != 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: enet_address_set_host failed for relay_host='%s'", relay_host);
        return 0;
    }
    relay_addr.port = ctrl_port;

    ENetSocket sock = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    if (sock == ENET_SOCKET_NULL)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: failed to create socket for relay CTRL");
        return 0;
    }

    enet_socket_set_option(sock, ENET_SOCKOPT_NONBLOCK, 1);

    uint16_t token_len = (uint16_t)strlen(token);
    if (token_len == 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: token length is 0");
        enet_socket_destroy(sock);
        return 0;
    }

    size_t pkt_len = 4 + 1 + 1 + 2 + token_len + 2 + 1;
    uint8_t* pkt = (uint8_t*)malloc(pkt_len);
    if (!pkt)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: malloc failed for HELLO packet");
        enet_socket_destroy(sock);
        return 0;
    }

    size_t off = 0;
      // Packet: 'N''R''L''Y' [ver=1] [type=0x01] [tokenLen u16be] [token bytes] [local data port] [terminator]
    pkt[off++] = 'N'; pkt[off++] = 'R'; pkt[off++] = 'L'; pkt[off++] = 'Y';
    pkt[off++] = (uint8_t)NRLY_VERSION;
    pkt[off++] = (uint8_t)NRLY_MSG_HELLO;

    Net_Write16(token_len, &pkt[off]);
    off += 2;
    memcpy(&pkt[off], token, token_len); off += token_len;

    Net_Write16(local_data_port, &pkt[off]);
    off += 2;
    pkt[off++] = 0;

    ENetBuffer b;
    b.data = pkt;
    b.dataLength = pkt_len;

    uint32_t start = SDL_GetTicks();
    uint32_t last_send = 0;

    DebugMessage(M64MSG_INFO, "Netplay: sending HELLO to %s:%u (token_len=%u data_port=%u)",
        relay_host, (unsigned)ctrl_port, (unsigned)token_len, (unsigned)local_data_port);

    while (1)
    {
        uint32_t now = SDL_GetTicks();

        // Send every 500 ticks
        if (now - last_send >= 500)
        {
            int sent = enet_socket_send(sock, &relay_addr, &b, 1);
            if (sent < 0)
            {
                DebugMessage(M64MSG_ERROR, "Netplay: enet_socket_send failed (ctrl HELLO) sent=%d", sent);
            }
            last_send = now;
        }

        // Receive READY or ERROR
        uint8_t rx[64];
        ENetBuffer rb;
        rb.data = rx;
        rb.dataLength = sizeof(rx);
        ENetAddress from = {0};

        int r = enet_socket_receive(sock, &from, &rb, 1);
        if (r > 0)
        {
            // Ensures that the packet we got is from the server
            if (r >= 6 && rx[4] == (uint8_t)NRLY_VERSION &&
                rx[0]=='N' && rx[1]=='R' && rx[2]=='L' && rx[3]=='Y')
            {
                if (rx[5] == (uint8_t)NRLY_MSG_READY)
                {
                    DebugMessage(M64MSG_INFO, "Netplay: relay CTRL READY received");
                    free(pkt);
                    enet_socket_destroy(sock);
                    return 1;
                }
                if (rx[5] == (uint8_t)NRLY_MSG_ERROR && r >= 7)
                {
                    DebugMessage(M64MSG_ERROR, "Netplay: relay CTRL ERROR code=%u", (unsigned)rx[6]);
                    break;
                }
            }
            else
            {
                DebugMessage(M64MSG_WARNING, "Netplay: unexpected CTRL response len=%d", r);
            }
        }

        if (now - start > 120000)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: relay CTRL handshake timed out");
            break;
        }

        SDL_Delay(10);
    }

    free(pkt);
    enet_socket_destroy(sock);
    return 0;
}

static int relay_data_bind(ENetSocket enet_sock, const char* relay_host,
                           uint16_t data_port, const char* token)
{
    ENetAddress relay_addr = {0};

    if (!token || token[0] == '\0')
    {
        DebugMessage(M64MSG_ERROR, "Netplay: DATA_BIND token is empty");
        return 0;
    }

    if (enet_address_set_host(&relay_addr, relay_host) != 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: DATA_BIND resolve failed for '%s'", relay_host);
        return 0;
    }
    relay_addr.port = data_port;

    uint16_t token_len = (uint16_t)strlen(token);

    // Packet: 'N''R''L''Y' [ver=1] [type=0x10] [tokenLen u16be] [token bytes]
    size_t pkt_len = 4 + 1 + 1 + 2 + token_len;
    uint8_t* pkt = (uint8_t*)malloc(pkt_len);
    if (!pkt)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: DATA_BIND malloc failed");
        return 0;
    }

    size_t off = 0;
    pkt[off++] = 'N'; pkt[off++] = 'R'; pkt[off++] = 'L'; pkt[off++] = 'Y';
    pkt[off++] = (uint8_t)NRLY_VERSION;
    pkt[off++] = (uint8_t)NRLY_MSG_DATA_BIND;
    Net_Write16(token_len, &pkt[off]);
    off += 2;
    memcpy(&pkt[off], token, token_len);
    off += token_len;

    ENetBuffer b;
    b.data = pkt;
    b.dataLength = pkt_len;

    DebugMessage(M64MSG_INFO, "Netplay: sending DATA_BIND to %s:%u (token_len=%u)",
                 relay_host, (unsigned)data_port, (unsigned)token_len);

    // Send a few times
    int ok = 0;
    for (int i = 0; i < 3; ++i)
    {
        int sent = enet_socket_send(enet_sock, &relay_addr, &b, 1);
        if (sent > 0)
            ok = 1;
        SDL_Delay(20);
    }

    free(pkt);
    return ok;
}