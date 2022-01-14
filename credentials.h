/*

Credentials file

*/

#pragma once

// Only one of these settings must be defined
#define USE_ABP
//#define USE_OTAA

#ifdef USE_ABP

    // LoRaWAN NwkSKey, network session key 
    static const u1_t PROGMEM NWKSKEY[16] = { 0xC5, 0xC2, 0x27, 0x64, 0x24, 0x2A, 0x68, 0x56, 0x54, 0x03, 0xD8, 0x9B, 0x9A, 0xA6, 0x7E, 0x84 };
    // LoRaWAN AppSKey, application session key 
    static const u1_t PROGMEM APPSKEY[16] = { 0xF6, 0x09, 0x71, 0x69, 0xD1, 0xC6, 0xF0, 0x40, 0x07, 0x76, 0x24, 0x63, 0xDE, 0x04, 0xE4, 0x69 };
    // LoRaWAN end-device address (DevAddr)
    static const u4_t DEVADDR = 0x260B276F;
    
#endif

#ifdef USE_OTAA

    // This EUI must be in little-endian format, so least-significant-byte
    // first. When copying an EUI from ttnctl output, this means to reverse
    // the bytes. For TTN issued EUIs the last bytes should be 0x00, 0x00,
    // 0x00.    0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x04, 0xA4, 0x5E
    static const u1_t PROGMEM APPEUI[8]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    // This should also be in little endian format, see above.
    // Note: You do not need to set this field, if unset it will be generated automatically based on the device macaddr}
    // fffefcbf46f23a08   0x70, 0xB3, 0xD5, 0x7E, 0xD0, 0x04, 0x9A, 0xAF
    static u1_t DEVEUI[8]  = { 0xAF, 0xA9, 0x04, 0xD0, 0x7E, 0xD5, 0xB3, 0x70 };

    // This key should be in big endian format (or, since it is not really a
    // number but a block of memory, endianness does not really apply). In
    // practice, a key taken from ttnctl can be copied as-is.
    // The key shown here is the semtech default key.
    static const u1_t PROGMEM APPKEY[16] = { 0x2F, 0x8A, 0x47, 0xD3, 0x75, 0x53, 0x58, 0x50, 0xEB, 0x05, 0x4D, 0xB8, 0x9F, 0x9E, 0x3F, 0x89 }; 

    
#endif
