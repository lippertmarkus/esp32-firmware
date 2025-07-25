/* esp32-firmware
 * Copyright (C) 2020-2021 Erik Fleckstein <erik@tinkerforge.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "cm_networking.h"

#include <fcntl.h>
#include <lwip/sockets.h>
#include <cstring>

#include "event_log_prefix.h"
#include "module_dependencies.h"
#include "tools.h"
#include "tools/net.h"
#include "modules/meters/meter_defs.h"

int CMNetworking::create_socket(uint16_t port, bool blocking)
{
    int sock;
    struct sockaddr_in dest_addr;

    dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        logger.printfln("Unable to create socket for port %hu: %s (%d)", port, strerror(errno), errno);
        return -1;
    }

    int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err < 0) {
        logger.printfln("Socket unable to bind to port %hu: %s (%d)", port, strerror(errno), errno);
        return -1;
    }

    if (blocking)
        return sock;

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        logger.printfln("Failed to get flags from socket for port %hu: %s (%d)", port, strerror(errno), errno);
        return -1;
    }

    err = fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    if (err < 0) {
        logger.printfln("Failed to set O_NONBLOCK flag for port %hu: %s (%d)", port, strerror(errno), errno);
        return -1;
    }

    return sock;
}

static const uint8_t cm_command_packet_length_versions[] = {
    sizeof(struct cm_packet_header),
    sizeof(struct cm_packet_header) + sizeof(struct cm_command_v1),
    sizeof(struct cm_packet_header) + sizeof(struct cm_command_v2), // cm_command_v2 redefined v1._padding to v2.allocated_phases. Size is still the same and cm_command_packet holds a union of v1 or v2.
};
static_assert(ARRAY_SIZE(cm_command_packet_length_versions) == (CM_COMMAND_VERSION + 1), "Unexpected amount of command packet length versions.");

static const uint8_t cm_state_packet_length_versions[] = {
    sizeof(struct cm_packet_header),
    sizeof(struct cm_packet_header) + sizeof(struct cm_state_v1),
    sizeof(struct cm_packet_header) + sizeof(struct cm_state_v1) + sizeof(struct cm_state_v2),
    sizeof(struct cm_packet_header) + sizeof(struct cm_state_v1) + sizeof(struct cm_state_v2) + sizeof(struct cm_state_v3),
};
static_assert(ARRAY_SIZE(cm_state_packet_length_versions) == (CM_STATE_VERSION + 1), "Unexpected amount of state packet length versions.");

static String validate_packet_header_common(const struct cm_packet_header *header, ssize_t recv_length, const uint8_t packet_length_versions[], const char *packet_type_name)
{
    if (recv_length < sizeof(struct cm_packet_header)) {
        return String("Truncated header with ") + recv_length + " bytes.";
    }

    if (header->magic != CM_PACKET_MAGIC) {
        return String("Invalid magic. Got ") + header->magic + '.';
    }

    return String();
}

static String validate_protocol_version(const struct cm_packet_header *header, uint8_t min_version, uint8_t max_known_version, const uint8_t packet_length_versions[], const char *packet_type_name, ssize_t recv_length) {
    if (header->version < min_version) {
        return String("Protocol version ") + header->version + " too old. Need at least version " + min_version + ".";
    }

    if (header->version <= max_known_version) { // Known protocol version; match against known packet length.
        if (header->length != packet_length_versions[header->version])
            return String("Invalid ") + packet_type_name + " packet length for known protocol version " + header->version + ": " + header->length + " bytes. Expected " + packet_length_versions[header->version] + " bytes.";

        // This is a known version. The recv_buf was large enough to receive the complete packet. Enforce length correctness
        if (recv_length != header->length)
            return String("Received truncated ") + packet_type_name + " packet for known protocol version " + header->version + ": " + recv_length + '/' + header->length + " bytes.";

    } else { // Newer protocol than known; packet must be at least as long as our newest known version.
        if (header->length < packet_length_versions[max_known_version])
            return String("Invalid ") + packet_type_name + " packet length for protocol version " + header->version + " from the future: " + header->length + " bytes.";

        // Received packet must be truncated because of the buffer size, other truncations are errors.
        if (recv_length != packet_length_versions[max_known_version])
            return String("Received truncated ") + packet_type_name + " packet for protocol version " + header->version + ": " + recv_length + '/' + header->length + " bytes.";
    }
    return "";
}

static String validate_command_packet_header(const struct cm_command_packet *pkt, ssize_t recv_length)
{
    String result = validate_packet_header_common(&(pkt->header), recv_length, cm_command_packet_length_versions, "command");
    if (!result.isEmpty())
        return result;

    return validate_protocol_version(&(pkt->header), CM_COMMAND_VERSION_MIN, CM_COMMAND_VERSION, cm_command_packet_length_versions, "command", recv_length);
}

static String validate_state_packet_header(const struct cm_state_packet *pkt, ssize_t recv_length)
{
    String result = validate_packet_header_common(&(pkt->header), recv_length, cm_state_packet_length_versions, "state");
    if (!result.isEmpty())
        return result;

    return validate_protocol_version(&(pkt->header), CM_STATE_VERSION_MIN, CM_STATE_VERSION, cm_state_packet_length_versions, "state", recv_length);
}

static bool seq_num_invalid(uint16_t received_sn, uint16_t last_seen_sn)
{
    return received_sn <= last_seen_sn && last_seen_sn - received_sn < 5;
}

static bool endswith(const char *haystack, const char *needle)
{
    size_t haystack_len = strlen(haystack);
    size_t needle_len = strlen(needle);

    if (haystack_len < needle_len)
        return false;

    return memcmp(haystack + haystack_len - needle_len, needle, needle_len) == 0;
}

struct ManagerTaskArgs {
    int manager_sock;
    QueueHandle_t manager_queue;
};

struct ManagerQueueItem {
    int len;
    struct cm_state_packet state_pkt;
    struct sockaddr_in source_addr;
};

#define CM_MANAGER_TASK_STACK_SIZE 1536

struct ManagerTaskData {
    StaticQueue_t xQueueBuffer;
    StaticTask_t xTaskBuffer;
    ManagerTaskArgs args;
    StackType_t xStack[CM_MANAGER_TASK_STACK_SIZE];
};

static void manager_task(void *arg)
{
    ManagerQueueItem item;
    memset(&item, 0, sizeof(ManagerQueueItem));

    auto manager_sock = ((ManagerTaskArgs *)arg)->manager_sock;
    auto manager_queue = ((ManagerTaskArgs *)arg)->manager_queue;

    for (;;) {
        socklen_t socklen = sizeof(item.source_addr);
        item.len = recvfrom(manager_sock, &item.state_pkt, sizeof(item.state_pkt), 0, (sockaddr *)&item.source_addr, &socklen);
        if (item.len == -1)
            item.len = -errno;

        // If the queue is full, just drop the item.
        xQueueSendToBack(manager_queue, &item, 0);
    }
}

void CMNetworking::register_manager(const char *const *const hosts,
                                    size_t device_count,
                                    const std::function<void(uint8_t /* client_id */, cm_state_v1 *, cm_state_v2 *, cm_state_v3 *)> &manager_callback,
                                    const std::function<void(uint8_t, uint8_t)> &manager_error_callback)
{
    const size_t sz = offsetof(struct manager_data_t, managed_devices) + sizeof(manager_data->managed_devices[0]) * device_count;
    manager_data = static_cast<decltype(manager_data)>(malloc(sz));
    if (!manager_data) {
        logger.printfln("Cannoc allocate memory for manager data");
        return;
    }

    manager_data->dns_resolver_active = false;
    manager_data->connected = false;
    manager_data->managed_device_count = device_count;

    for (size_t i = 0; i < device_count; ++i) {
        managed_device_data *device = manager_data->managed_devices + i;
        const char *hostname = hosts[i];

        device->hostname = hostname;
        device->last_resolve_attempt = 0;
        device->device_index = i;

        device->addr.sin_len    = sizeof(device->addr);
        device->addr.sin_family = AF_INET; // IPv4 only
        device->addr.sin_port   = htons(CHARGE_MANAGEMENT_PORT);

        ip4_addr_t ip4_addr;
        if (ip4addr_aton(hostname, &ip4_addr)) {
            // Hostname is actually an IPv4 address that never needs resolving.
            device->addr.sin_addr.s_addr = ip4_addr.addr;

            device->host_address_type = HostAddressType::IP;
            device->resolve_state     = ResolveState::Resolved;
        } else {
            device->addr.sin_addr.s_addr = 0;

            if (endswith(hostname, ".local")) {
                device->host_address_type = HostAddressType::mDNS;
                device->resolve_state     = ResolveState::NotResolved;
                device->mdns_hostname_len = strlen(hostname) - 6;
            } else {
                device->host_address_type = HostAddressType::DNS;
                device->resolve_state     = ResolveState::Unknown;
            }
        }
    }

    manager_data->manager_sock = create_socket(CHARGE_MANAGER_PORT, true);
    if (manager_data->manager_sock < 0) {
        logger.printfln("Failed to create manager socket");
        return;
    }

    // LWIP stores LWIP_UDP_RECVMBOX_SIZE (configured to 6)
    // UDP packets in the socket's receive buffer.
    // Use a separate task to receive state packets
    // to free the receive mbox as fast as possible.
    // The tasks resources may be leaked, because
    // it will run forever.

    ManagerTaskData *task_data = static_cast<ManagerTaskData *>(calloc_dram(1, sizeof(ManagerTaskData)));
    if (!task_data) {
        logger.printfln("Failed to allocate task data");
        return;
    }

    uint8_t *queue_storage = static_cast<uint8_t *>(calloc_psram_or_dram(manager_data->managed_device_count, sizeof(ManagerQueueItem)));
    if (!queue_storage) {
        logger.printfln("Failed to allocate queue storage");
        free(task_data);
        return;
    }

    QueueHandle_t manager_queue = xQueueCreateStatic(
        manager_data->managed_device_count,
        sizeof(ManagerQueueItem),
        queue_storage,
        &task_data->xQueueBuffer);

    task_data->args.manager_sock  = manager_data->manager_sock;
    task_data->args.manager_queue = manager_queue;

    TaskHandle_t xTask = xTaskCreateStatic(
        manager_task,
        "cm_manager_recv",
        sizeof(task_data->xStack),
        &task_data->args,
        ESP_TASK_TCPIP_PRIO - 1,
        task_data->xStack,
        &task_data->xTaskBuffer);

    #if MODULE_DEBUG_AVAILABLE()
        debug.register_task(xTask, sizeof(task_data->xStack));
    #else
        (void)xTask;
    #endif

    task_scheduler.scheduleWithFixedDelay([this, manager_callback, manager_error_callback, manager_queue](){
        static uint16_t last_seen_seq_num[MAX_CONTROLLED_CHARGERS];
        static bool initialized = false;
        if (!initialized) {
            memset(last_seen_seq_num, 255, sizeof(last_seen_seq_num));
            initialized = true; // FIXME: delayed initialization doesn't show in frontend
        }

        ManagerQueueItem item;

        // Try to receive up to four packets in one go to catch up on the backlog.
        // Don't receive every available packet to smooth out bursts of packets.
        static_assert(MAX_CONTROLLED_CHARGERS <= 64);
        for (int poll_ctr = 0; poll_ctr < 10; ++poll_ctr) {
            if (!xQueueReceive(manager_queue, &item, 0))
                return;

            int len = item.len;
            struct cm_state_packet &state_pkt = item.state_pkt;
            struct sockaddr_in &source_addr = item.source_addr;

            if (len < 0) {
                if (len != -EAGAIN && len != -EWOULDBLOCK)
                    logger.printfln("recvfrom failed: %s", strerror(-len));
                return;
            }

            int charger_idx = -1;
            for (int idx = 0; idx < this->manager_data->managed_device_count; ++idx) {
                managed_device_data *device = manager_data->managed_devices + idx;

                if (source_addr.sin_family      == device->addr.sin_family      &&
                    source_addr.sin_addr.s_addr == device->addr.sin_addr.s_addr &&
                    source_addr.sin_port        == device->addr.sin_port) {
                        charger_idx = idx;
                        break;
                }
            }

            // Don't log in the first 20 seconds after startup: We are probably still resolving hostnames.
            if (charger_idx == -1) {
                if (deadline_elapsed(20_s)) {
                    char source_str[16];
                    tf_ip4addr_ntoa(&source_addr, source_str, sizeof(source_str));

                    logger.printfln("Received packet from unknown %s. Is the config complete?", source_str);
                }
                return;
            }

            String validation_error = validate_state_packet_header(&state_pkt, len);
            if (!validation_error.isEmpty()) {
                char source_str[16];
                tf_ip4addr_ntoa(&source_addr, source_str, sizeof(source_str));

                logger.printfln("Received state packet from %s (%s) (%i bytes) failed validation: %s",
                                charge_manager.get_charger_name(charger_idx),
                                source_str,
                                len,
                                validation_error.c_str());
                if (manager_error_callback) {
                    manager_error_callback(charger_idx, CM_NETWORKING_ERROR_INVALID_HEADER);
                }
                return;
            }

            if (seq_num_invalid(state_pkt.header.seq_num, last_seen_seq_num[charger_idx])) {
                char source_str[16];
                tf_ip4addr_ntoa(&source_addr, source_str, sizeof(source_str));

                logger.printfln("Received stale (out of order?) state packet from %s (%s). Last seen seq_num is %u, Received seq_num is %u",
                                charge_manager.get_charger_name(charger_idx),
                                source_str,
                                last_seen_seq_num[charger_idx],
                                state_pkt.header.seq_num);
                return;
            }

            last_seen_seq_num[charger_idx] = state_pkt.header.seq_num;

            if (!CM_STATE_FLAGS_MANAGED_IS_SET(state_pkt.v1.state_flags)) {
                char source_str[16];
                tf_ip4addr_ntoa(&source_addr, source_str, sizeof(source_str));

                logger.printfln("%s (%s) reports managed is not activated!",
                    charge_manager.get_charger_name(charger_idx),
                    source_str);
                if (manager_error_callback) {
                    manager_error_callback(charger_idx, CM_NETWORKING_ERROR_NOT_MANAGED);
                }
                return;
            }

#if MODULE_EM_PHASE_SWITCHER_AVAILABLE()
            em_phase_switcher.filter_state_packet(charger_idx, &state_pkt);
#endif

            if (manager_callback) {
                manager_callback(charger_idx, &state_pkt.v1, state_pkt.header.version >= 2 ? &state_pkt.v2 : nullptr, state_pkt.header.version >= 3 ? &state_pkt.v3 : nullptr);
            } else {
                this->send_state_packet(&state_pkt);
            }
        }
    }, 50_ms, 50_ms);

#if MODULE_NETWORK_AVAILABLE()
    // Must schedule a task because this runs before the REGISTER_EVENTS stage.
    task_scheduler.scheduleOnce([this]() {
        network.on_network_connected([this](const Config *connected_cfg) {
            const bool connected = connected_cfg->asBool();

            this->manager_data->connected = connected;

            if (connected) {
                this->resolve_all();
            }

            return EventResult::OK;
        });
    });
#else
    manager_data->connected = true;
    task_scheduler.scheduleOnce([this]() {
        this->resolve_all();
    }, 2_s);
#endif
}

bool CMNetworking::send_manager_update(uint8_t client_id, uint16_t allocated_current, bool cp_disconnect_requested, int8_t allocated_phases)
{
    static uint16_t next_seq_num = 1;

    struct cm_command_packet command_pkt;
    command_pkt.header.magic = CM_PACKET_MAGIC;
    command_pkt.header.length = CM_COMMAND_PACKET_LENGTH;
    command_pkt.header.seq_num = next_seq_num;
    ++next_seq_num;
    command_pkt.header.version = CM_COMMAND_VERSION;

    command_pkt.v1.allocated_current = allocated_current;
    command_pkt.v1.command_flags = cp_disconnect_requested << CM_COMMAND_FLAGS_CPDISC_BIT_POS;

    command_pkt.v2.allocated_phases = allocated_phases;

    return send_command_packet(client_id, &command_pkt);
}

bool CMNetworking::send_command_packet(uint8_t client_id, cm_command_packet *command_pkt)
{
    if (!manager_data || manager_data->manager_sock < 0)
        return true;

    if (!is_resolved(client_id)) {
        return true;
    }

#if MODULE_EM_PHASE_SWITCHER_AVAILABLE()
    em_phase_switcher.filter_command_packet(client_id, command_pkt);
#endif

    int err = sendto(manager_data->manager_sock, command_pkt, sizeof(decltype(*command_pkt)), MSG_DONTWAIT, (sockaddr *)&manager_data->managed_devices[client_id].addr, sizeof(manager_data->managed_devices[client_id].addr));

    if (err < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            // Intentionally don't increment here, we want to resend to this charger next.
            return false;
        if (errno == ENOMEM) {
            // Ignore ENOMEM for now. Usually indicates that we don't have a network connection yet.
            return true;
        }

        if (errno == EHOSTUNREACH && manager_data->connected)
            logger.printfln("Failed to send command: %s (%d)", strerror(errno), errno);

        return true;
    }
    if (err != CM_COMMAND_PACKET_LENGTH) {
        logger.printfln("Failed to send command: sendto truncated packet (of %u bytes) to %d bytes.", CM_COMMAND_PACKET_LENGTH, err);
        return true;
    }
    return true;
}

void CMNetworking::register_client(const std::function<void(uint16_t, bool, int8_t)> &client_callback)
{
    client_sock = create_socket(CHARGE_MANAGEMENT_PORT, false);

    if (client_sock < 0)
        return;

    memset(&manager_addr, 0, sizeof(manager_addr));

    task_scheduler.scheduleWithFixedDelay([this, client_callback](){
        static uint16_t last_seen_seq_num = 255;
        static micros_t last_successful_recv = now_us();

        struct cm_command_packet command_pkt;

        struct sockaddr_storage from_addr;
        socklen_t socklen = sizeof(from_addr);
        int len = recvfrom(client_sock, &command_pkt, sizeof(command_pkt), 0, (struct sockaddr *)&from_addr, &socklen);

        if (len < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                logger.printfln("recvfrom failed: errno %d", errno);

            // If we have not received a valid packet for one minute, invalidate manager_addr.
            // Otherwise we would send state packets to this address forever.
            if (deadline_elapsed(last_successful_recv + 60_s))
                manager_addr_valid = false;

            return;
        }

        String validation_error = validate_command_packet_header(&command_pkt, len);
        if (!validation_error.isEmpty()) {
            char from_str[16];
            tf_ip4addr_ntoa(&from_addr, from_str, sizeof(from_str));
            logger.printfln("Received command packet from %s (%i bytes) failed validation: %s", from_str, len, validation_error.c_str());
            return;
        }

        if (seq_num_invalid(command_pkt.header.seq_num, last_seen_seq_num)) {
            logger.printfln("Received stale (out of order?) command packet. last seen seq_num is %u, received seq_num is %u", last_seen_seq_num, command_pkt.header.seq_num);
            return;
        }

        last_seen_seq_num = command_pkt.header.seq_num;

        if (memcmp(&this->manager_addr, &from_addr, from_addr.s2_len) != 0) {
            char manager_str[16];
            char from_str[16];
            tf_ip4addr_ntoa(&this->manager_addr, manager_str, sizeof(manager_str));
            tf_ip4addr_ntoa(&from_addr,          from_str,    sizeof(from_str   ));

            if (deadline_elapsed(this->last_manager_addr_change + 1_min)) {
                if (this->manager_addr.s2_len > 0) {
                    logger.printfln("Manager address changed from %s to %s", manager_str, from_str);
                }
                this->manager_addr_valid = true;
            } else {
                logger.printfln("Rejecting conflicting manager address change from %s to %s", manager_str, from_str);
                this->manager_addr_valid = false;
            }

            memcpy(&this->manager_addr, &from_addr, from_addr.s2_len);
            this->last_manager_addr_change = now_us();

            if (!this->manager_addr_valid) {
                // Block charging
                if (client_callback) {
                    client_callback(0, false, 0);
                }

                return;
            }
        } else { // Manager address unchanged
            if (!this->manager_addr_valid && this->manager_addr.s2_len > 0) {
                if (deadline_elapsed(this->last_manager_addr_change + 1_min)) {
                    char manager_str[16];
                    tf_ip4addr_ntoa(&this->manager_addr, manager_str, sizeof(manager_str));

                    logger.printfln("Accepting manager address %s", manager_str);
                    this->manager_addr_valid = true;
                } else {
                    // Block charging
                    if (client_callback) {
                        client_callback(0, false, 0);
                    }

                    return;
                }
            }
        }

        last_successful_recv = now_us();

        if (client_callback) {
            client_callback(command_pkt.v1.allocated_current,
                            CM_COMMAND_FLAGS_CPDISC_IS_SET(command_pkt.v1.command_flags),
                            command_pkt.header.version >= 2 ? command_pkt.v2.allocated_phases : 0);
        } else {
            this->send_command_packet(0, &command_pkt);
        }
        //logger.printfln("Received command packet. Allocated current is %u", command_pkt.v1.allocated_current);
    }, 100_ms, 100_ms);
}

bool CMNetworking::send_client_update(uint32_t esp32_uid,
                                      uint8_t iec61851_state,
                                      uint8_t charger_state,
                                      uint32_t time_since_state_change,
                                      uint8_t error_state,
                                      uint32_t uptime,
                                      uint32_t car_stopped_charging,
                                      uint16_t allowed_charging_current,
                                      uint16_t supported_current,
                                      bool managed,
                                      bool cp_disconnected_state,
                                      int8_t phases,
                                      bool can_switch_phases_now)
{
    static uint16_t next_seq_num = 0;

    if (!manager_addr_valid) {
        //logger.printfln("Manager addr not valid.");
        return false;
    }
    //logger.printfln("Sending state packet.");

    struct cm_state_packet state_pkt;
    state_pkt.header.magic = CM_PACKET_MAGIC;
    state_pkt.header.length = CM_STATE_PACKET_LENGTH;
    state_pkt.header.seq_num = next_seq_num;
    ++next_seq_num;
    state_pkt.header.version = CM_STATE_VERSION;

    bool has_phase_switch = api.hasFeature("phase_switch");
    bool has_meter_values = api.hasFeature("meter_all_values");
    bool has_meter_phases = api.hasFeature("meter_phases");
    bool has_meter        = api.hasFeature("meter");

    state_pkt.v1.feature_flags = 0
        | has_phase_switch                          << CM_FEATURE_FLAGS_PHASE_SWITCH_BIT_POS
        | api.hasFeature("cp_disconnect")           << CM_FEATURE_FLAGS_CP_DISCONNECT_BIT_POS
        | api.hasFeature("evse")                    << CM_FEATURE_FLAGS_EVSE_BIT_POS
        | api.hasFeature("nfc")                     << CM_FEATURE_FLAGS_NFC_BIT_POS
        | has_meter_values                          << CM_FEATURE_FLAGS_METER_ALL_VALUES_BIT_POS
        | has_meter_phases                          << CM_FEATURE_FLAGS_METER_PHASES_BIT_POS
        | has_meter                                 << CM_FEATURE_FLAGS_METER_BIT_POS
        | api.hasFeature("button_configuration")    << CM_FEATURE_FLAGS_BUTTON_CONFIGURATION_BIT_POS;

    state_pkt.v1.esp32_uid = esp32_uid;
    state_pkt.v1.evse_uptime = uptime;
    state_pkt.v1.car_stopped_charging = car_stopped_charging;
    state_pkt.v1.allowed_charging_current = allowed_charging_current;
    state_pkt.v1.supported_current = supported_current;
    state_pkt.v1.iec61851_state = iec61851_state;
    state_pkt.v1.charger_state = charger_state;
    state_pkt.v1.error_state = error_state;

    long flags = 0
        | managed               << CM_STATE_FLAGS_MANAGED_BIT_POS
        | cp_disconnected_state << CM_STATE_FLAGS_CP_DISCONNECTED_BIT_POS;
    if (has_meter_phases) {
        auto meter_phase_values = api.getState("meter/phases");
        flags |= meter_phase_values->get("phases_connected")->get(0)->asBool() << CM_STATE_FLAGS_L1_CONNECTED_BIT_POS;
        flags |= meter_phase_values->get("phases_connected")->get(1)->asBool() << CM_STATE_FLAGS_L2_CONNECTED_BIT_POS;
        flags |= meter_phase_values->get("phases_connected")->get(2)->asBool() << CM_STATE_FLAGS_L3_CONNECTED_BIT_POS;
        flags |= meter_phase_values->get("phases_active")->get(0)->asBool() << CM_STATE_FLAGS_L1_ACTIVE_BIT_POS;
        flags |= meter_phase_values->get("phases_active")->get(1)->asBool() << CM_STATE_FLAGS_L2_ACTIVE_BIT_POS;
        flags |= meter_phase_values->get("phases_active")->get(2)->asBool() << CM_STATE_FLAGS_L3_ACTIVE_BIT_POS;
    }
    state_pkt.v1.state_flags = static_cast<uint8_t>(flags);

    if (has_meter_values) {
        auto meter_all_values = api.getState("meter/all_values");
        for (size_t i = 0; i < 3; i++) {
            state_pkt.v1.line_voltages[i]      = meter_all_values->get(i + METER_ALL_VALUES_LINE_TO_NEUTRAL_VOLTS_L1)->asFloat();
            state_pkt.v1.line_currents[i]      = meter_all_values->get(i + METER_ALL_VALUES_CURRENT_L1_A            )->asFloat();
            state_pkt.v1.line_power_factors[i] = meter_all_values->get(i + METER_ALL_VALUES_POWER_FACTOR_L1         )->asFloat();
        }
    } else {
        for (int i = 0; i < 3; i++) {
            state_pkt.v1.line_voltages[i]      = 0;
            state_pkt.v1.line_currents[i]      = 0;
            state_pkt.v1.line_power_factors[i] = 0;
        }
    }

    if (has_meter) {
        auto meter_values = api.getState("meter/values");
        state_pkt.v1.power_total = meter_values->get("power")->asFloat();
        state_pkt.v1.energy_rel = meter_values->get("energy_rel")->asFloat();
        state_pkt.v1.energy_abs = meter_values->get("energy_abs")->asFloat();
    } else {
        state_pkt.v1.power_total = 0;
        state_pkt.v1.energy_rel = 0;
        state_pkt.v1.energy_abs = 0;
    }

    state_pkt.v2.time_since_state_change = time_since_state_change;

    state_pkt.v3.phases = phases;
    state_pkt.v3.phases |= can_switch_phases_now << CM_STATE_V3_CAN_PHASE_SWITCH_BIT_POS;

    return send_state_packet(&state_pkt);
}

bool CMNetworking::send_state_packet(const cm_state_packet *state_pkt)
{
    if (!manager_addr_valid) {
        //logger.printfln("Manager addr not valid.");
        return false;
    }

    int err = sendto(client_sock, state_pkt, sizeof(decltype(*state_pkt)), 0, (sockaddr *)&manager_addr, sizeof(manager_addr));
    if (err < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            logger.printfln("Failed to send state: %s (%d)", strerror(errno), errno);
        return false;
    }
    if (err != CM_STATE_PACKET_LENGTH) {
        logger.printfln("Failed to send state: sendto truncated packet (of %u bytes) to %d bytes.", CM_STATE_PACKET_LENGTH, err);
        return false;
    }

    return true;
}
