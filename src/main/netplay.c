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


static int l_canFF;
static int l_netplay_controller;
static int l_netplay_control[4];
static struct netplay_event* l_early_events[4]; // Buffer for packets received before registration
static ENetHost* l_host = NULL;
static ENetPeer* l_peer = NULL;
static int l_spectator;
static int l_netplay_is_init = 0;
static uint32_t l_vi_counter;
static uint8_t l_status;
static uint32_t l_reg_id;
static struct controller_input_compat *l_cin_compats;
static uint8_t l_plugin[4];
static uint8_t l_buffer_target;
static uint8_t l_player_lag[4];

//Packet data buffer for reassembly if needed
static uint8_t* l_incoming_data = NULL;
static size_t l_incoming_size = 0;
static int l_is_server = 0;
static int l_client_ready = 0;

static const int32_t l_check_sync_packet_size = (CP0_REGS_COUNT * 4) + 5;

//Packet formats
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

// Disconnect helper
static void disconnect_and_cleanup(void);


m64p_error netplay_start(const char* host, int port)
{
    l_is_server = (host && (_stricmp(host, "server") == 0 || _stricmp(host, "host") == 0));

    if (enet_initialize() != 0)
    {
        DebugMessage(M64MSG_ERROR, "Netplay: An error occurred while initializing ENet.");
        return M64ERR_SYSTEM_FAIL;
    }

    if (l_is_server)
    {
        ENetAddress address;
        address.host = ENET_HOST_ANY;
        address.port = port;

        // Create server host (32 incoming connections, 2 channels, 0 up/down bandwidth limits)
        l_host = enet_host_create(&address, 32, 2, 0, 0);
        if (l_host == NULL)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: An error occurred while trying to create an ENet server host on port %d.", port);
            return M64ERR_SYSTEM_FAIL;
        }

        DebugMessage(M64MSG_INFO, "Netplay: Server started on port %d. Waiting for client...", port);

        // Wait for a client to connect (Blocking up to 60 seconds?)
        // Since we are inside the main thread potentially before emulation loop or during init, blocking is risky but necessary for 1v1 setup.
        ENetEvent event;
        int connected = 0;
        uint32_t start_time = SDL_GetTicks();
        
        while ((SDL_GetTicks() - start_time) < 30000) // 30s timeout
        {
            if (enet_host_service(l_host, &event, 100) > 0)
            {
                if (event.type == ENET_EVENT_TYPE_CONNECT)
                {
                    DebugMessage(M64MSG_INFO, "Netplay: Client connected from %x:%u", event.peer->address.host, event.peer->address.port);
                    l_peer = event.peer;
                    connected = 1;
                    break;
                }
            }
        }

        if (!connected)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: No client connected within timeout.");
            enet_host_destroy(l_host);
            l_host = NULL;
            return M64ERR_SYSTEM_FAIL;
        }
    }
    else
    {
        // Create client host (1 outgoing connection, 2 channels, 0 up/down bandwidth limits)
        l_host = enet_host_create(NULL, 1, 2, 0, 0);
        if (l_host == NULL)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: An error occurred while trying to create an ENet client host.");
            return M64ERR_SYSTEM_FAIL;
        }

        ENetAddress address = { 0 };
        if (enet_address_set_host(&address, host) != 0)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: Failed to resolve hostname %s", host);
            enet_host_destroy(l_host);
            l_host = NULL;
            return M64ERR_SYSTEM_FAIL;
        }
        address.port = port;

        // Initiate connection
        l_peer = enet_host_connect(l_host, &address, 2, 0);
        if (l_peer == NULL)
        {
            DebugMessage(M64MSG_ERROR, "Netplay: No available peers for initiating an ENet connection.");
            enet_host_destroy(l_host);
            l_host = NULL;
            return M64ERR_SYSTEM_FAIL;
        }

        // Wait up to 5 seconds for the connection to succeed
        ENetEvent event;
        int serviceResult = enet_host_service(l_host, &event, 5000);
        
        if (serviceResult > 0 && event.type == ENET_EVENT_TYPE_CONNECT)
        {
            DebugMessage(M64MSG_INFO, "Netplay: Connection to %s:%d succeeded.", host, port);
        }
        else
        {
            if (serviceResult == 0)
                DebugMessage(M64MSG_ERROR, "Netplay: Connection to %s:%d timed out.", host, port);
            else if (serviceResult > 0)
                DebugMessage(M64MSG_ERROR, "Netplay: Connection failed with event type %d.", event.type);
            else
                DebugMessage(M64MSG_ERROR, "Netplay: An error occurred while servicing the host.");

            enet_peer_reset(l_peer);
            enet_host_destroy(l_host);
            l_host = NULL;
            l_peer = NULL;
            return M64ERR_SYSTEM_FAIL;
        }
    }

    for (int i = 0; i < 4; ++i)
    {
        l_netplay_control[i] = -1;
        l_plugin[i] = 0;
        l_player_lag[i] = 0;
        l_early_events[i] = NULL;
    }

    l_canFF = 0;
    l_netplay_controller = 0;
    l_netplay_is_init = 1;
    l_spectator = 1;
    l_vi_counter = 0;
    l_status = 0;
    l_reg_id = 0;
    l_incoming_data = NULL;
    l_incoming_size = 0;

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
    
    // Cleanup early buffer
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
        
        // Force reset if minimal nice disconnect failed
        enet_peer_reset(l_peer);
    }
    
disconnect_success:
    enet_host_destroy(l_host);
    enet_deinitialize();
    
    l_host = NULL;
    l_peer = NULL;
    l_is_server = 0;
    l_netplay_is_init = 0;
    
    if (l_incoming_data) free(l_incoming_data);
    l_incoming_data = NULL;
}

static uint8_t buffer_size(uint8_t control_id)
{
    //This function returns the size of the local input buffer
    uint8_t counter = 0;
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current != NULL)
    {
        current = current->next;
        ++counter;
    }
    return counter;
}

static int check_valid(uint8_t control_id, uint32_t count)
{
    //Check if we already have this event recorded locally, returns 1 if we do
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current != NULL)
    {
        if (current->count == count) //event already recorded
            return 1;
        current = current->next;
    }
    return 0;
}

// Replaces netplay_process
static void netplay_poll(void)
{
    ENetEvent event;
    // Only poll if we don't have pending data waiting to be consumed
    if (l_incoming_data != NULL) return;

    while (enet_host_service(l_host, &event, 0) > 0)
    {
        switch (event.type)
        {
            case ENET_EVENT_TYPE_CONNECT:
                // New connection? We aren't a server, so we shouldn't get many of these except our own init
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                DebugMessage(M64MSG_ERROR, "Netplay: Disconnected from server.");
                l_netplay_is_init = 0; // Trigger shutdown handling
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                // Handle packet
                {
                    uint8_t* data = event.packet->data;
                    size_t len = event.packet->dataLength;
                    
                    if (len < 1) { enet_packet_destroy(event.packet); continue; }

                    // Debug Log for packet tracing
                    /* if (!l_is_server && data[0] == PACKET_RECEIVE_KEY_INFO) // Log only Key Info on client
                         DebugMessage(M64MSG_INFO, "Netplay Cli: Recv Key Info Len %zu", len); */

                    switch (data[0])
                    {
                        case PACKET_REGISTER_PLAYER: // Server side handler
                            if (l_is_server)
                            {
                                if (len < 8) break;
                                // Client registering. ACK with [player_id, buffer_target]
                                uint8_t player_id = data[1];
                                uint8_t resp[2] = { player_id, 5 }; // Buffer target 5?
                                ENetPacket* p = enet_packet_create(resp, 2, ENET_PACKET_FLAG_RELIABLE);
                                enet_peer_send(event.peer, 0, p);
                            }
                            break;
                        case PACKET_CLIENT_READY:
                            if (l_is_server)
                            {
                                // DebugMessage(M64MSG_INFO, "Netplay: Client Ready");
                                l_client_ready = 1;
                            }
                            break;
                        case PACKET_GET_REGISTRATION: // Server side handler
                            if (l_is_server)
                            {
                                // DebugMessage(M64MSG_INFO, "Netplay: Client requested registration");
                                // Send 24 bytes of registration info WRAPPED in packet
                                uint8_t resp[25] = {0};
                                resp[0] = PACKET_RECEIVE_REGISTRATION;
                                
                                uint32_t curr = 1;
                                // P1 (Server)
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
                        case PACKET_SEND_KEY_INFO: // Server side handler
                            if (l_is_server)
                            {
                                if (len < 11) break;
                                // Client sent input.
                                uint8_t player = data[1];
                                uint32_t count = Net_Read32(&data[2]);
                                uint32_t keys = Net_Read32(&data[6]);
                                uint8_t plugin = data[10];

                                // Validation: Player ID must be valid (1 for P2?)
                                if (player > 3) break;

                                // Validation: Check l_cin_compats pointer safety
                                if (l_cin_compats == NULL) 
                                {
                                     // This can happen if a packet arrives before 'netplay_read_registration' calls 'l_cin_compats = ...' 
                                     // or after 'main_run' exits.
                                     // Discard.
                                     break;
                                }

                                // Store locally
                                // Check pointer before dereference
                                struct controller_input_compat* pComp = &l_cin_compats[player];
                                
                                if (((count - pComp->netplay_count) <= (UINT32_MAX / 2)) && (!check_valid(player, count)))
                                {
                                    struct netplay_event* new_event = (struct netplay_event*)malloc(sizeof(struct netplay_event));
                                    new_event->count = count;
                                    new_event->buttons = keys;
                                    new_event->plugin = plugin;
                                    new_event->next = pComp->event_first;
                                    pComp->event_first = new_event;
                                
                                    // Echo to Client
                                    uint8_t echo_data[14];
                                    echo_data[0] = PACKET_RECEIVE_KEY_INFO;
                                    echo_data[1] = player; // P2's input
                                    echo_data[2] = l_status;
                                    echo_data[3] = 0;
                                    echo_data[4] = 1;
                                    Net_Write32(count, &echo_data[5]);
                                    Net_Write32(keys, &echo_data[9]);
                                    echo_data[13] = plugin;
                                    ENetPacket* p = enet_packet_create(echo_data, 14, ENET_PACKET_FLAG_RELIABLE);
                                    enet_peer_send(event.peer, 1, p);
                                }
                            }
                            break;
                            
                        case PACKET_RECEIVE_KEY_INFO:
                        {
                            if (len < 6) break;
                            uint8_t player = data[1];
                            uint32_t count_dbg = Net_Read32(&data[5]);
                            
                            uint8_t current_status = data[2];
                            uint8_t lag = data[3];
                            uint8_t count_events = data[4];

                            // Validation: Player ID
                            if (player > 3) break;
                            
                            l_player_lag[player] = lag;

                            // Buffer packets if l_cin_compats is not yet set
                            if (l_cin_compats == NULL)
                            {
                                 // DebugMessage(M64MSG_INFO, "Netplay: Buffering early input P%u Frame %u", player+1, count_dbg);
                                 
                                 size_t curr = 5;
                                 for (int i = 0; i < count_events; ++i)
                                 {
                                     if (curr + 9 > len) break;
                                     uint32_t count = Net_Read32(&data[curr]);
                                     curr += 4;
                                     uint32_t keys = Net_Read32(&data[curr]);
                                     curr += 4;
                                     uint8_t plugin = data[curr];
                                     curr += 1;
                                     
                                     // Insert into early buffer
                                     struct netplay_event* new_event = (struct netplay_event*)malloc(sizeof(struct netplay_event));
                                     new_event->count = count;
                                     new_event->buttons = keys;
                                     new_event->plugin = plugin;
                                     // Append at head (LIFO reversed? No, we will append properly flush)
                                     // Actually, if we just push to Head, they are in reverse order of arrival?
                                     // Wait, 'netplay_get_input' searches regardless of order.
                                     // So order doesn't matter for correctness, only efficiency.
                                     // But if we receive duplicate counts, we filter.
                                     // Here we don't filter (no l_cin_count to check against).
                                     // We just stuff them all in.
                                     
                                     new_event->next = l_early_events[player];
                                     l_early_events[player] = new_event;
                                 }
                                 break;
                            }

                            // Validation: l_cin_compats must be valid
                            // (Redundant check if we handled NULL above, but kept for clarity/safety of below code)
                            if (l_cin_compats == NULL)
                            {
                                break;
                            }
                            
                            if (current_status != l_status)
                            {
                                if (((current_status & 0x1) ^ (l_status & 0x1)) != 0)
                                    DebugMessage(M64MSG_ERROR, "Netplay: players have de-synced at VI %u", l_vi_counter);
                                for (int dis = 1; dis < 5; ++dis)
                                {
                                    if (((current_status & (0x1 << dis)) ^ (l_status & (0x1 << dis))) != 0)
                                        DebugMessage(M64MSG_ERROR, "Netplay: player %u has disconnected", dis);
                                }
                                l_status = current_status;
                            }
                            
                            size_t curr = 5;
                            for (int i = 0; i < count_events; ++i)
                            {
                                if (curr + 9 > len) break;
                                
                                uint32_t count = Net_Read32(&data[curr]);
                                curr += 4;
                                uint32_t keys = Net_Read32(&data[curr]);
                                curr += 4;
                                uint8_t plugin = data[curr];
                                curr += 1;
                                
                                // Double check pointer safey
                                if (l_cin_compats == NULL) break;

                                struct controller_input_compat* pComp = &l_cin_compats[player];

                                if (check_valid(player, count))
                                {
                                     continue;
                                }

                                if ((count - pComp->netplay_count) > (UINT32_MAX / 2))
                                {
                                     continue;
                                }
                                
                                // Insert event
                                struct netplay_event* new_event = (struct netplay_event*)malloc(sizeof(struct netplay_event));
                                new_event->count = count;
                                new_event->buttons = keys;
                                new_event->plugin = plugin;
                                
                                // Insert at tail? Or Head?
                                // The original code inserted at Head. 'next = event_first'.
                                // But `netplay_get_input` searches the list linearly.
                                // Order implies LIFO?
                                // If packets come out of order, we need to sort?
                                // TCP/Reliable guarantees order.
                                // So we insert at Head. 
                                // BUT: If we insert 0, then 1. List: 1 -> 0.
                                // `get_input` search for 0. Found.
                                // Then search for 1. Found.
                                // This works.
                                
                                new_event->next = pComp->event_first;
                                pComp->event_first = new_event;
                            }
                            break;
                        }
                        // Other packet types processed in their specific contexts if blocking,
                        // or ignoring them here if handled elsewhere?
                        // For a clean ENet loop, we should ideally handle ALL packets here or put them in a queue.
                        // However, strictly preserving the existing blocking logic for some ops (like getting registration) 
                        // might be easier if we just ignore them here and consume them in the specialized functions.
                        // BUT ENet doesn't allow "peeking" easily without servicing. 
                        // We will buffer unexpected packets if we need to implement blocking waits.
                        default:
                            if (l_incoming_data == NULL)
                            {
                                l_incoming_data = malloc(len);
                                memcpy(l_incoming_data, data, len);
                                l_incoming_size = len;
                                // Stop processing events so we don't overwrite this packet
                                enet_packet_destroy(event.packet);
                                return;
                            }
                            else
                            {
                                // This should strictly not happen if we check l_incoming_data at top of function
                                // But if a logic error occurs...
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


static void netplay_delete_event(struct netplay_event* current, uint8_t control_id)
{
    //This function deletes an event from the linked list
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

static uint32_t netplay_get_input(uint8_t control_id)
{
    uint32_t keys = 0;
    
    // Safety: If there is leftover blob data during gameplay, it's garbage/desync.
    // Discard it so we don't deadlock netplay_poll.
    if (l_incoming_data != NULL)
    {
         DebugMessage(M64MSG_WARNING, "Netplay: Discarding unexpected data blob during input loop (%zu bytes)", l_incoming_size);
         free(l_incoming_data); 
         l_incoming_data = NULL;
    }
    
    // Process incoming packets
    netplay_poll();

    // With ENet, we don't need to manually request retransmission (reliable packets).
    // We just wait for the data to arrive.
    // However, we MUST service the host to get the packets.

    //l_buffer_target is set by the server upon registration
    //l_player_lag is how far behind we are from the lead player
    //buffer_size is the local buffer size
    if (l_player_lag[control_id] > 0 && buffer_size(control_id) > l_buffer_target)
    {
        l_canFF = 1;
        main_core_state_set(M64CORE_SPEED_LIMITER, 0);
    }
    else
    {
        main_core_state_set(M64CORE_SPEED_LIMITER, 1);
        l_canFF = 0;
    }

    // Wait for validity (Blocking, but servicing ENet)
    uint32_t timeout = SDL_GetTicks() + 10000;
    while (!check_valid(control_id, l_cin_compats[control_id].netplay_count))
    {
        // Safety check inside the wait loop too
        if (l_incoming_data != NULL)
        {
             free(l_incoming_data); 
             l_incoming_data = NULL;
        }

        if (!l_netplay_is_init || SDL_GetTicks() > timeout)
        {
             DebugMessage(M64MSG_ERROR, "Netplay: Timed out waiting for input (or lost connection).");
             main_core_state_set(M64CORE_EMU_STATE, M64EMU_STOPPED);
             return 0;
        }
        
        // We aren't requesting input anymore, just servicing the connection
        netplay_poll();
        SDL_Delay(1);
    }

    // Found it
    struct netplay_event* current = l_cin_compats[control_id].event_first;
    while (current->count != l_cin_compats[control_id].netplay_count)
        current = current->next;
    keys = current->buttons;
    Controls[control_id].Plugin = current->plugin;
    netplay_delete_event(current, control_id);
    ++l_cin_compats[control_id].netplay_count;

    return keys;
}

static void netplay_send_input(uint8_t control_id, uint32_t keys)
{
    if (l_is_server)
    {
         // We are the server (P1 usually). We have generated input 'keys' for 'control_id'.
         
         // Store it locally immediately.
         if (!check_valid(control_id, l_cin_compats[control_id].netplay_count)) 
         {
             struct netplay_event* new_event = (struct netplay_event*)malloc(sizeof(struct netplay_event));
             new_event->count = l_cin_compats[control_id].netplay_count;
             new_event->buttons = keys;
             new_event->plugin = l_plugin[control_id]; // assume generic
             new_event->next = l_cin_compats[control_id].event_first;
             l_cin_compats[control_id].event_first = new_event;
         }
         
         // Broadcast to Client
         // Note: We used to broadcast in the 'if !check_valid' block.
         // But if we generated it, we WANT to broadcast it.
         // Wait, if it was 'valid', it means we ALREADY generated it?
         // Is it possible to generate the same frame twice?
         // Yes, if update_pif_ram is called twice for field/frame?
         // Standard N64 is 60VI/s. One Update per VI.
         // If we rebroadcast, it's safer than dropping?
         // Clients handle duplicates.
         // But let's stick to only broadcasting NEW events.
         
         uint8_t echo_data[14];
         echo_data[0] = PACKET_RECEIVE_KEY_INFO;
         echo_data[1] = control_id;
         echo_data[2] = l_status; 
         echo_data[3] = 0; 
         echo_data[4] = 1; 
         Net_Write32(l_cin_compats[control_id].netplay_count, &echo_data[5]);
         Net_Write32(keys, &echo_data[9]);
         echo_data[13] = l_plugin[control_id];
         
         ENetPacket* p = enet_packet_create(echo_data, 14, ENET_PACKET_FLAG_RELIABLE);
         enet_peer_send(l_peer, 0, p);
         enet_host_flush(l_host);
    }
    else
    {
        uint8_t data[11];
        data[0] = PACKET_SEND_KEY_INFO;
        data[1] = control_id;
        Net_Write32(l_cin_compats[control_id].netplay_count, &data[2]);
        Net_Write32(keys, &data[6]);
        data[10] = l_plugin[control_id];

        // Use Channel 1 for Input (Reliable)
        ENetPacket* packet = enet_packet_create(data, 11, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(l_peer, 1, packet);
        
        // Flush periodically or rely on auto-flush? 
        // Usually enet_host_service flushes, but we want to send NOW.
        enet_host_flush(l_host);
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
        if (l_incoming_data && l_incoming_size >= 2) // Assuming raw bytes come in buffer
        {
             // Verify it's what we want? 
             // In this simple port, we assume the first thing hitting the "default" buffer is our response.
             // The server response for register is just 2 raw bytes in previous code, not a packet ID?
             // Actually, the previous code used TCP raw stream.
             // WE NEED TO CHANGE THE SERVER TO SEND PACKETS WITH IDs if we were rewriting server too.
             // Assuming we wrap the response in a packet or just parse the data. 
             // Let's assume the server sends the same 2 bytes but inside an ENet packet.
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
    //This function syncs save games.
    const char *file_extension = strrchr(filename, '.');
    file_extension += 1;

    uint32_t buffer_pos = 0;
    char *output_data = malloc(size + strlen(file_extension) + 6);
    
    ENetPacket* packet;

    file_status_t ret;
    uint8_t request;

    if (l_is_server) 
    {
        // Host: Wait for Client Request first
        // Clear temp buffer
        if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
        
        uint32_t start = SDL_GetTicks();
        int got_request = 0;
        
        // Host waits up to 30s for the client to ask for THIS specific file.
        // NOTE: If Client crashes or desyncs, Host waits.
        while ((SDL_GetTicks() - start) < 30000)
        {
            netplay_poll();
            if (l_incoming_data)
            {
                if (l_incoming_data[0] == PACKET_RECEIVE_SAVE)
                {
                    // Check if extension matches?
                    // The request packet format: [ID(1)] [ExtString(\0)]
                    // We assume sequential processing so order matches.
                    got_request = 1;
                    free(l_incoming_data); l_incoming_data = NULL;
                    break;
                }
                else
                {
                     // Wrong packet? Ignore and free to continue polling?
                     // If we get Settings packet while waiting for Save, that's bad sync.
                     // For now, discard.
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

        //extension of the file we are requesting
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
                            
                            // Check for "no save" indicator? (All zeros)
                            // The host sends all zeros if file didn't exist.
                            // We check sum.
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

    // Use Host mode flag to determine authority, OR existing controller logic if already set?
    // At this point (main_run), controller logic might not be set yet if we haven't done registration?
    // netplay_sync_settings is called at L1750, netplay_read_registration is L1844.
    // So controller mapping is NOT set yet.
    // We must rely on l_is_server.

    char output_data[SETTINGS_SIZE + 1];
    uint8_t request;
    
    // Server sends settings, Client receives
    if (l_is_server) 
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
        // Client waits for settings
        // Wait for response (Server pushes it, or we request and wait?)
        // In the old code, client requested REQUEST_SETTINGS or something?
        // Old code: if (l_netplay_control[0] != -1) SEND else RECEIVE
        
        // Let's assume server just Sends them proactively or we need to handshake.
        // To be safe, let's just wait for the packet. Host will send it.
        
        // Wait for response
        if (l_incoming_data) { free(l_incoming_data); l_incoming_data = NULL; }
        
        uint32_t start = SDL_GetTicks();
        while ((SDL_GetTicks() - start) < 5000)
        {
            netplay_poll();
            if (l_incoming_data)
            {
                if (l_incoming_data[0] == PACKET_SEND_SETTINGS && l_incoming_size >= SETTINGS_SIZE + 1)
                {
                    // Correctly skip the header byte (Packet ID)
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
                    // Wrong packet (maybe delayed save packet?), discard
                    free(l_incoming_data); l_incoming_data = NULL;
                }
            }
            SDL_Delay(10);
        }
    }
}

void netplay_check_sync(struct cp0* cp0)
{
    //This function is used to check if games have desynced
    //Every 600 VIs, it sends the value of the CP0 registers to the server
    //The server will compare the values, and update the status byte if it detects a desync
    if (!netplay_is_init())
        return;

    if (l_vi_counter % 600 == 0)
    {
        const uint32_t* cp0_regs = r4300_cp0_regs(cp0);

        uint8_t data[ (CP0_REGS_COUNT * 4) + 5 ];

        data[0] = PACKET_SYNC_DATA;
        Net_Write32(l_vi_counter, &data[1]); //current VI count
        for (int i = 0; i < CP0_REGS_COUNT; ++i)
        {
            Net_Write32(cp0_regs[i], &data[(i * 4) + 5]);
        }
        
        ENetPacket* packet = enet_packet_create(data, (CP0_REGS_COUNT * 4) + 5, ENET_PACKET_FLAG_RELIABLE);
        enet_peer_send(l_peer, 0, packet);
        // enet_host_flush(l_host); // Optional, can be aggregated 
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
                
                // Add to cin_compats list
                // Since this is init, cin_compats list is likely empty.
                // But let's just push to Head.
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
    
    // Set global pointer EARLY so we can receive inputs while waiting for registration.
    l_cin_compats = cin_compats;
    
    // Flush any buffered inputs
    netplay_flush_early_buffer();

    if (l_is_server)
    {
        // We are the server, we define the registration.
        // For this simple 1v1 port, we hardcode:
        // P1 = Local (Server)
        // P2 = Remote (Client)
        // P3/P4 = Empty
        
        // P1
        Controls[0].Present = 1;
        Controls[0].Plugin = PLUGIN_NONE; // Standard
        Controls[0].RawData = 0;
        l_plugin[0] = PLUGIN_NONE;
        netplay_set_controller(0); // We control P1

        //P2 (Client)
        Controls[1].Present = 1;
        Controls[1].Plugin = PLUGIN_NONE;
        Controls[1].RawData = 0;
        l_plugin[1] = PLUGIN_NONE;
        // P2 is remote, we don't 'set_controller' (which implies local control)

        // P3/P4
        Controls[2].Present = 0;
        Controls[2].Plugin = PLUGIN_NONE;
        Controls[3].Present = 0;
        Controls[3].Plugin = PLUGIN_NONE;
        
        // Host waits for client to be ready
        uint32_t start = SDL_GetTicks();
        while ((SDL_GetTicks() - start) < 30000) 
        {
             netplay_poll();
             if (l_client_ready) break;
             SDL_Delay(10);
        }
        if (!l_client_ready) DebugMessage(M64MSG_ERROR, "Netplay: Timed out waiting for client ready.");
        
        return;
    }

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
             // Wrong packet?
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

        if (reg_id == 0) //No one registered to control this player
        {
            Controls[i].Present = 0;
            Controls[i].Plugin = PLUGIN_NONE;
            Controls[i].RawData = 0;
            curr += 2;
        }
        else
        {
            Controls[i].Present = 1;
            if (i > 0 && input_data[curr] == PLUGIN_MEMPAK) // only P1 can use mempak
                Controls[i].Plugin = PLUGIN_NONE;
            else if (input_data[curr] == PLUGIN_TRANSFER_PAK) // Transferpak not supported during netplay
                Controls[i].Plugin = PLUGIN_NONE;
            else
                Controls[i].Plugin = input_data[curr];
            l_plugin[i] = Controls[i].Plugin;
            ++curr;
            Controls[i].RawData = input_data[curr];
            ++curr;
            
            // If the reg_id matches our requested ID (Client is P2), assume control?
            // In the simplified server logic, P2 is always the client.
            if (i == 1) // Hardcode Client = P2
            {
               netplay_set_controller(1);
            }
        }
    }
    
    // Explicitly zero out P3 and P4 to ensure no memory pak ghosting if server said they are empty
    for (int i=0; i<4; ++i)
    {
        if (Controls[i].Present == 0)
        {
             Controls[i].Plugin = PLUGIN_NONE;
        }
    }
    
    // Send Ready Signal
    output_data = PACKET_CLIENT_READY;
    packet = enet_packet_create(&output_data, 1, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(l_peer, 0, packet);
    enet_host_flush(l_host);
    
    // DebugMessage(M64MSG_INFO, "Netplay: Client Registration Complete. Sending Ready.");
}

static void netplay_send_raw_input(struct pif* pif)
{
    for (int i = 0; i < 4; ++i)
    {
        if (l_netplay_control[i] != -1)
        {
            if (pif->channels[i].tx && pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
                netplay_send_input(i, *(uint32_t*)pif->channels[i].rx_buf);
        }
    }
}

static void netplay_get_raw_input(struct pif* pif)
{
    for (int i = 0; i < 4; ++i)
    {
        if (Controls[i].Present == 1)
        {
            if (pif->channels[i].tx)
            {
                *pif->channels[i].rx &= ~0xC0; //Always show the controller as connected

                if(pif->channels[i].tx_buf[0] == JCMD_CONTROLLER_READ)
                {
                    *(uint32_t*)pif->channels[i].rx_buf = netplay_get_input(i);
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
