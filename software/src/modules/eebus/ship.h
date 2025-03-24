/* esp32-firmware
 * Copyright (C) 2024 Olaf Lüke <olaf@tinkerforge.com>
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

#pragma once

#include <vector>

#include "module.h"
#include "config.h"
#include "modules/ws/web_sockets.h"
#include "ship_connection.h"
#include "mdns.h"
#include <TFJson.h>
#include "string_builder.h"

struct ShipNode {

    // Basic information about the node
    std::vector<IPAddress> ip_addresses;
    uint16_t port;
    bool registered = false;
    bool connected = false;
    // Stuff that is Mandatory in the TXT record
    String dns_name;
    String txt_vers; //Maybe change to number?
    String txt_id;
    String txt_wss_path;
    String txt_ski; 
    bool txt_autoregister;
    // Stuff that is Optional in the TXT record
    String txt_brand = "";
    String txt_model = "";
    String txt_type = "";

    // TODO Add more stuff that might be relevant like last seen, features, etc.

    void as_json(StringBuilder *sb) {
        char json_buf[1024]; //TODO: Use 1024 for now, change later to dynamic size depending on struct size
        TFJsonSerializer json(json_buf, sizeof(json_buf));
        json.addMemberString("dns_name", dns_name.c_str());
        json.addMemberString("txt_vers", txt_vers.c_str());
        json.addMemberString("txt_id", txt_id.c_str());
        json.addMemberString("txt_wss_path", txt_wss_path.c_str());
        json.addMemberString("txt_ski", txt_ski.c_str());
        json.addMemberBoolean("txt_autoregister", txt_autoregister);
        json.addMemberString("txt_brand", txt_brand.c_str());
        json.addMemberString("txt_model", txt_model.c_str());
        json.addMemberString("txt_type", txt_type.c_str());
        
        StringBuilder ip_sb;
        ip_sb.putc('[');
        for (IPAddress ip : ip_addresses) {
            ip_sb.puts(ip.toString().c_str());
            ip_sb.putc(',');
        }
        ip_sb.putc(']');
        json.addMemberString("ip_addresses", ip_sb.getPtr());
        json.end();
        sb->puts(json_buf);
    }
    
};


class Ship
{
private:
    void setup_mdns();
    void setup_wss();

    WebSockets web_sockets;
    std::vector<ShipConnection> ship_connections;





public:
    Ship(){}
    
    void pre_setup();
    void setup();
    void remove(const ShipConnection &ship_connection);
    void scan_skis();
    void print_skis(StringBuilder *sb);
    void scan_skis();
    void print_skis(StringBuilder *sb);

    ConfigRoot config;
    ConfigRoot state;

    std::vector<ShipNode> mdns_results;
};
