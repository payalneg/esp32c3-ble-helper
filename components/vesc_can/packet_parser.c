#include "vesc_can/packet_parser.h"
#include "vesc_can/crc.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "packet_parser";

void packet_parser_init(packet_parser_t *parser)
{
    parser->state           = PARSER_STATE_IDLE;
    parser->is_long_packet  = false;
    parser->payload_length  = 0;
    parser->bytes_received  = 0;
    parser->crc_received    = 0;
    memset(parser->buffer, 0, sizeof(parser->buffer));
}

void packet_parser_reset(packet_parser_t *parser)
{
    packet_parser_init(parser);
}

bool packet_parser_process_byte(packet_parser_t *parser, uint8_t byte,
                                packet_processed_callback_t callback)
{
    switch (parser->state) {
    case PARSER_STATE_IDLE:
        if (byte == PACKET_START_BYTE_SHORT) {
            parser->is_long_packet = false;
            parser->state          = PARSER_STATE_LENGTH;
            parser->payload_length = 0;
            parser->bytes_received = 0;
            parser->crc_received   = 0;
        } else if (byte == PACKET_START_BYTE_LONG) {
            parser->is_long_packet = true;
            parser->state          = PARSER_STATE_LENGTH_HIGH;
            parser->payload_length = 0;
            parser->bytes_received = 0;
            parser->crc_received   = 0;
        }
        break;

    case PARSER_STATE_LENGTH:
        parser->payload_length = byte;
        if (parser->payload_length == 0 ||
            parser->payload_length > sizeof(parser->buffer)) {
            ESP_LOGW(TAG, "invalid short payload length: %u",
                     (unsigned)parser->payload_length);
            packet_parser_reset(parser);
            return false;
        }
        parser->state          = PARSER_STATE_PAYLOAD;
        parser->bytes_received = 0;
        break;

    case PARSER_STATE_LENGTH_HIGH:
        parser->payload_length = (uint16_t)byte << 8;
        parser->state          = PARSER_STATE_LENGTH_LOW;
        break;

    case PARSER_STATE_LENGTH_LOW:
        parser->payload_length |= byte;
        if (parser->payload_length == 0 ||
            parser->payload_length > sizeof(parser->buffer)) {
            ESP_LOGW(TAG, "invalid long payload length: %u",
                     (unsigned)parser->payload_length);
            packet_parser_reset(parser);
            return false;
        }
        parser->state          = PARSER_STATE_PAYLOAD;
        parser->bytes_received = 0;
        break;

    case PARSER_STATE_PAYLOAD:
        parser->buffer[parser->bytes_received++] = byte;
        if (parser->bytes_received >= parser->payload_length) {
            parser->state = PARSER_STATE_CRC_HIGH;
        }
        break;

    case PARSER_STATE_CRC_HIGH:
        parser->crc_received = (uint16_t)byte << 8;
        parser->state        = PARSER_STATE_CRC_LOW;
        break;

    case PARSER_STATE_CRC_LOW:
        parser->crc_received |= byte;
        parser->state         = PARSER_STATE_END_BYTE;
        break;

    case PARSER_STATE_END_BYTE: {
        if (byte != PACKET_END_BYTE) {
            ESP_LOGW(TAG, "bad end byte 0x%02X (expected 0x%02X)",
                     byte, PACKET_END_BYTE);
            packet_parser_reset(parser);
            return false;
        }

        uint16_t calc = crc16(parser->buffer, parser->payload_length);
        if (calc != parser->crc_received) {
            ESP_LOGW(TAG, "CRC mismatch: calc=0x%04X recv=0x%04X",
                     calc, parser->crc_received);
            packet_parser_reset(parser);
            return false;
        }

        if (callback) {
            callback(parser->buffer, parser->payload_length);
        }
        packet_parser_reset(parser);
        return true;
    }
    }

    return false;
}

uint16_t packet_build_frame(const uint8_t *payload, uint16_t payload_len,
                            uint8_t *out_buffer, uint16_t out_buffer_size)
{
    if (payload_len == 0 || payload_len > PACKET_PARSER_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "build_frame: bad payload_len=%u", (unsigned)payload_len);
        return 0;
    }

    bool     use_long      = (payload_len > 255);
    uint16_t total_length  = use_long
                                 ? (uint16_t)(1 + 2 + payload_len + 2 + 1)
                                 : (uint16_t)(1 + 1 + payload_len + 2 + 1);

    if (total_length > out_buffer_size) {
        ESP_LOGW(TAG, "build_frame: out_buffer too small (need %u, have %u)",
                 (unsigned)total_length, (unsigned)out_buffer_size);
        return 0;
    }

    uint16_t ind = 0;
    out_buffer[ind++] = use_long ? PACKET_START_BYTE_LONG
                                 : PACKET_START_BYTE_SHORT;
    if (use_long) {
        out_buffer[ind++] = (uint8_t)((payload_len >> 8) & 0xFF);
        out_buffer[ind++] = (uint8_t)(payload_len & 0xFF);
    } else {
        out_buffer[ind++] = (uint8_t)(payload_len & 0xFF);
    }
    memcpy(out_buffer + ind, payload, payload_len);
    ind += payload_len;

    uint16_t crc = crc16(payload, payload_len);
    out_buffer[ind++] = (uint8_t)((crc >> 8) & 0xFF);
    out_buffer[ind++] = (uint8_t)(crc & 0xFF);
    out_buffer[ind++] = PACKET_END_BYTE;

    return ind;
}
