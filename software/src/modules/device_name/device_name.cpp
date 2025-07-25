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
#include "device_name.h"

#include "event_log_prefix.h"
#include "module_dependencies.h"
#include "options.h"

extern char local_uid_str[32];

static const size_t options_hostname_prefix_length = constexpr_strlen(OPTIONS_HOSTNAME_PREFIX());

void DeviceName::pre_setup()
{
    name = Config::Object({
        {"name", Config::Str("", 0, options_hostname_prefix_length + 1 + ARRAY_SIZE(local_uid_str))},
        {"type", Config::Str(OPTIONS_HOSTNAME_PREFIX(), 0, options_hostname_prefix_length)},
        {"display_type", Config::Str("", 0, 64)},
        {"uid", Config::Str("", 0, 32)}
    });

    display_name = Config::Object({
        {"display_name", Config::Str("", 0, 32)}
    });
}

#if OPTIONS_PRODUCT_ID_IS_WARP() || OPTIONS_PRODUCT_ID_IS_WARP2() || OPTIONS_PRODUCT_ID_IS_WARP3() || OPTIONS_PRODUCT_ID_IS_WARP4() || OPTIONS_PRODUCT_ID_IS_ELTAKO()
String getWarpDisplayType(bool add_optional_hw=true)
{
    String display_type =
#if OPTIONS_PRODUCT_ID_IS_ELTAKO()
        ""; // ELTAKO Wallbox always has a meter
#else
        api.hasFeature("meter") ? " Pro" : " Smart";
#endif

    if (api.hasFeature("evse")) {
        display_type += api.getState("evse/slots")->get(1)->get("max_current")->asUint() <= 20000 ? " 11" : " 22";
        display_type += "kW";
    } else {
        display_type += " w/o EVSE";
    }

#if OPTIONS_PRODUCT_ID_IS_WARP()
    if (add_optional_hw && api.hasFeature("nfc")) {
        display_type += " +NFC";
    }
#endif

#if OPTIONS_PRODUCT_ID_IS_WARP() || OPTIONS_PRODUCT_ID_IS_WARP2()
    if (add_optional_hw && api.hasFeature("rtc")) {
        display_type += " +RTC";
    }
#endif
    return display_type;
}
#endif

static bool isVowel(char c)
{
    return (0x208222 >> (c & 0x1f)) & 1;
}

#if OPTIONS_PRODUCT_ID_IS_WARP() || OPTIONS_PRODUCT_ID_IS_WARP2() || OPTIONS_PRODUCT_ID_IS_WARP3() || OPTIONS_PRODUCT_ID_IS_WARP4() || OPTIONS_PRODUCT_ID_IS_ELTAKO()
String DeviceName::get20CharDisplayType() {
    String display_type = OPTIONS_PRODUCT_NAME();
    display_type += getWarpDisplayType(false);
#if OPTIONS_PRODUCT_ID_IS_ELTAKO()
    display_type.replace(" Wallbox", "");
#else
    display_type.replace(" Charger", "");
#endif
    display_type = display_type.substring(0, 20);
    return display_type;
}
#endif

void DeviceName::updateDisplayType()
{
    String display_type = OPTIONS_PRODUCT_NAME();
#if OPTIONS_PRODUCT_ID_IS_WARP() || OPTIONS_PRODUCT_ID_IS_WARP2() || OPTIONS_PRODUCT_ID_IS_WARP3() || OPTIONS_PRODUCT_ID_IS_WARP4() || OPTIONS_PRODUCT_ID_IS_ELTAKO()
    display_type += getWarpDisplayType(); // FIXME: Also add more details for WARP Energy Manager, similar to WARP[2] here?
#endif

    const char *indef_article = isVowel(display_type[0]) ? "an" : "a";

    if (name.get("display_type")->updateString(display_type)) {
        logger.printfln("This is %s (%s), %s %s", display_name.get("display_name")->asEphemeralCStr(), name.get("name")->asEphemeralCStr(), indef_article, display_type.c_str());
    }
}

void DeviceName::setup()
{
    name.get("name")->updateString(String(OPTIONS_HOSTNAME_PREFIX()) + "-" + local_uid_str);
    name.get("uid")->updateString(String(local_uid_str));

    // We intentionally don't use the display_name_in_use = display_name construction here:
    // We want to be able to change the display_name without a reboot, because in the web interface
    // we don't use the usual save + reboot modal code-path.
    if (!api.restorePersistentConfig("info/display_name", &display_name)) {
        display_name.get("display_name")->updateString(name.get("name")->asString());
    }

    task_scheduler.scheduleWithFixedDelay([this](){
        this->updateDisplayType();
    }, 60_s);

    initialized = true;
}

void DeviceName::register_urls()
{
    api.addState("info/name", &name);
    api.addPersistentConfig("info/display_name", &display_name);
}
