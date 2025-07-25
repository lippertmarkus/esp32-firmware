/* esp32-firmware
 * Copyright (C) 2025 Olaf Lüke <olaf@tinkerforge.com>
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

// IPv6/TCP/ISO-15118-20

#include "iso20.h"

void ISO20::pre_setup()
{
    api_state = Config::Object({
        {"state", Config::Uint8(0)},
    });
}

void ISO20::handle_bitstream(exi_bitstream *exi)
{
    // Increment state on first call
    if (state == 0) {
        state = 1;
    }


    api_state.get("state")->updateUint(state);
}