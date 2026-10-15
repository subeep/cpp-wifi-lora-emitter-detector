"""Open Drone ID v0/v1/v2 messages and receive transport envelopes.

Wire layout/conversions follow Open Drone ID Core C (see docs/REMOTE_ID.md).
Decoded values are transmitter assertions. Authentication pages are data only;
no signature, manufacturer, exact model or aircraft existence is inferred.
"""
import struct

OUI_TYPE = bytes.fromhex('fa0bbc0d')
NAN_SERVICE = bytes.fromhex('8869199d9209')
MESSAGE_SIZE = 25
MAX_MESSAGES = 9
SUPPORTED_VERSIONS = (0, 1, 2)
TYPE_NAMES = ('Basic ID', 'Location', 'Authentication', 'Self ID', 'System', 'Operator ID')
UA_TYPES = ('Unspecified', 'Aeroplane', 'Helicopter / multirotor', 'Gyroplane',
            'Hybrid lift', 'Ornithopter', 'Glider', 'Kite', 'Free balloon',
            'Captive balloon', 'Airship', 'Free fall parachute', 'Rocket',
            'Tethered powered aircraft', 'Ground obstacle', 'Other')
ID_TYPES = ('Unspecified', 'Serial number', 'CAA registration', 'UTM UUID', 'Session ID')
HORIZONTAL_BOUNDS_M = (None,18520,7408,3704,1852,926,555.6,185.2,92.6,30,10,3,1)
VERTICAL_BOUNDS_M = (None,150,45,25,10,3,1)
SPEED_BOUNDS_M_S = (None,10,3,1,.3)


def _category(code, names):
    return names[code] if code < len(names) else 'Reserved / unknown'


def _accuracy(code, bounds):
    return bounds[code] if code < len(bounds) else None


def _text(raw):
    value = raw.split(b'\0', 1)[0]
    return value.decode('ascii') if all(32 <= c < 127 for c in value) else None


def management_frame(mpdu, subtype):
    if len(mpdu) < 28:
        return False
    control = int.from_bytes(mpdu[:2], 'little')
    # Permit Retry/Power Management/More Data, reject protected/fragmented,
    # DS-addressed or ordered frames whose bodies/header need other handling.
    return control & 0xff == subtype and not control & 0xc700 and not mpdu[22] & 15


def _u16(raw, at):
    return int.from_bytes(raw[at:at + 2], 'little')


def _alt(raw, at):
    value = _u16(raw, at)
    return value * .5 - 1000 if value else None


def _position(raw, at):
    lat, lon = struct.unpack_from('<ii', raw, at)
    lat, lon = lat / 1e7, lon / 1e7
    ok = -90 <= lat <= 90 and -180 <= lon <= 180 and (lat, lon) != (0, 0)
    return dict(latitude=lat if ok else None, longitude=lon if ok else None,
                position_status='Reported' if ok else 'Unavailable / invalid',
                latitude_raw=struct.unpack_from('<i', raw, at)[0],
                longitude_raw=struct.unpack_from('<i', raw, at + 4)[0])


def decode_message(raw):
    if len(raw) != MESSAGE_SIZE or raw[0] & 15 not in SUPPORTED_VERSIONS or raw[0] >> 4 >= len(TYPE_NAMES):
        return None
    kind, version = raw[0] >> 4, raw[0] & 15
    m = dict(type=TYPE_NAMES[kind], protocol_version=version, raw_hex=raw.hex())
    if kind == 0:
        id_type, ua_type = raw[1] >> 4, raw[1] & 15
        if id_type not in ((1, 2, 3) if version == 0 else (1, 2, 3, 4)):
            return None
        m.update(id_type=id_type, id_type_name=ID_TYPES[id_type], ua_type=ua_type,
                 ua_type_name=UA_TYPES[ua_type], reported_id_hex=raw[2:22].hex())
        if id_type in (1, 2):
            label = _text(raw[2:22])
            if not label:
                return None
            m['reported_id'] = label
    elif kind == 1:
        direction = raw[2] + (180 if raw[1] & 2 else 0)
        speed_h = raw[3] * (.75 if raw[1] & 1 else .25) + (63.75 if raw[1] & 1 else 0)
        speed_v = struct.unpack_from('<b', raw, 4)[0] * .5
        stamp = _u16(raw, 21)
        m.update(_position(raw, 5))
        m.update(status=raw[1] >> 4, direction_deg=direction if direction <= 360 else None,
                 speed_horizontal_m_s=speed_h if speed_h <= 254.25 else None,
                 speed_vertical_m_s=speed_v if -62 <= speed_v <= 62 else None,
                 altitude_baro_m=_alt(raw, 13), altitude_geo_m=_alt(raw, 15),
                 height_m=_alt(raw, 17), height_reference='Ground' if raw[1] & 4 else 'Takeoff',
                 horizontal_accuracy_code=raw[19] & 15, vertical_accuracy_code=raw[19] >> 4,
                 speed_accuracy_code=raw[20] & 15, baro_accuracy_code=raw[20] >> 4,
                 timestamp_seconds_past_hour=stamp / 10 if stamp <= 36000 else None,
                 timestamp_accuracy_code=raw[23] & 15)
        status_names = ('Undeclared','On ground','Airborne','Emergency')
        if version == 2:
            status_names += ('Remote ID system failure',)
        m.update(status_name=_category(m['status'],status_names),
                 horizontal_accuracy_upper_bound_m=_accuracy(m['horizontal_accuracy_code'],HORIZONTAL_BOUNDS_M),
                 vertical_accuracy_upper_bound_m=_accuracy(m['vertical_accuracy_code'],VERTICAL_BOUNDS_M),
                 baro_accuracy_upper_bound_m=_accuracy(m['baro_accuracy_code'],VERTICAL_BOUNDS_M),
                 speed_accuracy_upper_bound_m_s=_accuracy(m['speed_accuracy_code'],SPEED_BOUNDS_M_S),
                 timestamp_accuracy_s=m['timestamp_accuracy_code']/10 if m['timestamp_accuracy_code'] else None)
    elif kind == 2:
        page = raw[1] & 15
        if version == 0 and page >= 5:
            return None
        m.update(auth_type=raw[1] >> 4, data_page=page, signature_verified=False)
        if page == 0:
            if version == 0:
                # Historical Core C preserves PageCount (including zero) and
                # bounds Length to five pages. Do not apply later index/FEC
                # semantics or manufacture a last-page index for this field.
                if raw[2] > 5 or raw[3] > 109:
                    return None
                m.update(page_count=raw[2], last_page_index=None,
                         page_metadata_status='Legacy PageCount; not a last-page index')
            else:
                if raw[2] > 15 or raw[3] > 17 + 23 * raw[2]:
                    return None
                m['last_page_index'] = raw[2]
            m.update(length=raw[3], timestamp_2019_s=int.from_bytes(raw[4:8], 'little'),
                     auth_data_hex=raw[8:].hex())
        else:
            m['auth_data_hex'] = raw[2:].hex()
    elif kind == 3:
        m.update(description_type=raw[1], description=_text(raw[2:]), description_hex=raw[2:].hex())
        descriptions = ('General text', 'Emergency details', 'Extended status') if version == 2 else ('General text',)
        m['description_type_name']=_category(raw[1], descriptions)
    elif kind == 4:
        m.update(_position(raw, 2))
        m.update(operator_location_type=raw[1] & 3, classification_type=(raw[1] >> 2) & 7,
                 area_count=_u16(raw, 10), area_radius_m=raw[12] * 10,
                 area_ceiling_m=_alt(raw, 13), area_floor_m=_alt(raw, 15),
                 category_eu=raw[17] >> 4, class_eu=raw[17] & 15,
                 operator_altitude_geo_m=_alt(raw, 18) if version >= 1 else None,
                 timestamp_2019_s=int.from_bytes(raw[20:24], 'little') if version == 2 else None)
        m['operator_altitude_geo_status'] = ('Not carried by protocol v0' if version == 0 else
                                            'Reported' if m['operator_altitude_geo_m'] is not None else 'Unavailable')
        m['timestamp_status'] = 'Reported' if version == 2 else f'Not carried by protocol v{version}'
        m['version_reserved_raw_hex'] = raw[18 if version == 0 else 20 if version == 1 else 24:].hex()
        m.update(operator_location_type_name=_category(m['operator_location_type'],('Takeoff location','Live GNSS location','Fixed location')),
                 classification_type_name=_category(m['classification_type'],('Undeclared','EU classification')),
                 category_eu_name=_category(m['category_eu'],('Undeclared','Open','Specific','Certified')) if m['classification_type']==1 else 'Not applicable / unknown')
    else:
        m.update(operator_id_type=raw[1], operator_id=_text(raw[2:22]), operator_id_hex=raw[2:22].hex())
    return m


def parse_payload(payload, transport, counter):
    """One 25-byte message (BLE) or bounded pack. Wi-Fi requires a pack."""
    if not payload or payload[0] & 15 not in SUPPORTED_VERSIONS:
        return None
    version = payload[0] & 15
    packed = bool(payload and payload[0] >> 4 == 15)
    if packed:
        if len(payload) < 3 or payload[1] != MESSAGE_SIZE:
            return None
        count = payload[2]
        limit = 10 if version == 0 else MAX_MESSAGES
        if not 1 <= count <= limit or len(payload) != 3 + count * MESSAGE_SIZE:
            return None
        raw_messages = [payload[3 + i * 25:3 + (i + 1) * 25] for i in range(count)]
    elif transport.startswith('Bluetooth') and len(payload) == MESSAGE_SIZE:
        raw_messages = [payload]
    else:
        return None
    messages, counts, auth_pages, ids = [], {}, set(), {}
    for raw in raw_messages:
        message = decode_message(raw)
        if message is None or message['protocol_version'] != version:
            return None
        kind = message['type']
        counts[kind] = counts.get(kind, 0) + 1
        kind_limit = (1 if version == 0 else 2) if kind == 'Basic ID' else (5 if version == 0 else 16) if kind == 'Authentication' else 1
        if counts[kind] > kind_limit:
            return None
        if kind == 'Authentication':
            key = message['auth_type'], message['data_page']
            if key in auth_pages:
                return None
            auth_pages.add(key)
        if kind == 'Basic ID':
            previous=ids.get(message['id_type'])
            if previous is not None and previous!=message['reported_id_hex']:
                return None  # Ambiguous same-type identities must not silently overwrite.
            ids[message['id_type']]=message['reported_id_hex']
        messages.append(message)
    return dict(transport=transport, protocol_version=version, message_counter=counter,
                message_count=len(messages), messages=messages,
                basic_ids=[m for m in messages if m['type'] == 'Basic ID'], authenticated=False)


def parse_vendor_body(body):
    if len(body) < 8 or body[:4] != OUI_TYPE:
        return None
    return parse_payload(body[5:], 'Wi-Fi Beacon', body[4])


def parse_nan_frame(mpdu):
    """FCS checked by caller. Parse bounded NAN service descriptors, no scanning."""
    if (len(mpdu) < 34 or not management_frame(mpdu, 0xd0) or
            mpdu[4:10] != bytes.fromhex('516f9a010000') or
            mpdu[24:30] != bytes.fromhex('0409506f9a13')):
        return []
    end, at, found = len(mpdu) - 4, 30, []
    while at < end:
        if at + 3 > end:
            return []
        kind, size = mpdu[at], _u16(mpdu, at + 1)
        at += 3
        if at + size > end:
            return []
        data = mpdu[at:at + size]
        at += size
        if kind != 3 or len(data) < 10 or data[:6] != NAN_SERVICE:
            continue
        # Remote ID reference follow-up descriptor: service info present,
        # no optional matching/response filters (different layouts unsupported).
        if data[8] != 0x10 or data[9] != len(data) - 10 or len(data) < 14:
            continue
        pack = parse_payload(data[11:], 'Wi-Fi NAN', data[10])
        if pack:
            found.append(pack)
    return found


def parse_ble_advertising(data):
    """GAP service-data UUID 0xFFFA + Open Drone ID app code 0x0D.

    Caller must obtain complete advertising data through CRC-checked USRP PHY.
    This also accepts complete extended AD data for future PHY backends; it
    does not implement Bluetooth 5 reception or fragment reassembly.
    """
    at, found = 0, []
    while at < len(data):
        size = data[at]
        if size == 0:
            return found if not any(data[at:]) else []
        if at + 1 + size > len(data):
            return []
        element = data[at + 1:at + 1 + size]
        at += size + 1
        if len(element) >= 5 and element[:4] == bytes.fromhex('16faff0d'):
            pack = parse_payload(element[5:], 'Bluetooth LE', element[4])
            if pack:
                found.append(pack)
    return found
