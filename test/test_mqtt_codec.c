#include <stdio.h>
#include <string.h>

#include "mqtt_codec.h"

/* Phase 17 focused MQTT codec tests: fixed header, Remaining Length (incl.
   edge cases and malformed encodings), and packet encode/decode round-trips. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static void test_remaining_length(void)
{
    printf("\n== Remaining Length ==\n");
    const uint32_t vals[] = {0U, 1U, 127U, 128U, 129U, 16383U, 16384U};
    const uint8_t  exp[]  =
    {
        0x00,
        0x01,
        0x7F,
        0x80, 0x01,
        0x81, 0x01,
        0xFF, 0x7F,
        0x80, 0x80, 0x01
    };
    size_t ei = 0;
    for (size_t i = 0; i < sizeof(vals)/sizeof(vals[0]); i++)
    {
        uint8_t enc[MQTT_RL_MAX_BYTES];
        size_t n = MqttCodec_WriteVarInt(enc, sizeof(enc), vals[i]);
        size_t expect = (vals[i] <= 127U) ? 1U : (vals[i] <= 16383U ? 2U : 3U);
        int match = (n == expect);
        if (match)
            for (size_t k = 0; k < n; k++)
                if (enc[k] != exp[ei + k]) { match = 0; break; }
        ei += expect;
        char name[48];
        snprintf(name, sizeof(name), "RL %lu encodes %u bytes canonically",
                 (unsigned long)vals[i], (unsigned)expect);
        T(match, name);

        uint32_t dec = 0; size_t consumed = 0;
        MqttCodecStatus rs = MqttCodec_ReadVarInt(enc, n, &consumed, &dec);
        T(rs == MQTT_CODEC_OK && dec == vals[i] && consumed == n, "round-trip decode");
    }

    /* WOULD_BLOCK: incomplete varint. */
    {
        uint8_t p[1] = {0x80};
        uint32_t v = 0; size_t c = 0;
        T(MqttCodec_ReadVarInt(p, 1, &c, &v) == MQTT_CODEC_WOULD_BLOCK, "incomplete RL -> WOULD_BLOCK");
    }
    /* Malformed: 5 continuation bytes. */
    {
        uint8_t p[5] = {0x80,0x80,0x80,0x80,0x80};
        uint32_t v = 0; size_t c = 0;
        T(MqttCodec_ReadVarInt(p, 5, &c, &v) == MQTT_CODEC_MALFORMED, "5-byte continuation -> MALFORMED");
    }
    /* Full-range varint decodes (generic primitive): 268435455 max legal. */
    {
        uint8_t p[4] = {0xFF,0xFF,0xFF,0x7F};   /* 268435455 */
        uint32_t v = 0; size_t c = 0;
        T(MqttCodec_ReadVarInt(p, 4, &c, &v) == MQTT_CODEC_OK && v == 268435455U,
          "generic varint decodes full 4-byte range");
    }
}

static void test_fixed_header(void)
{
    printf("\n== Fixed header ==\n");
    /* PINGREQ: 0xC0 0x00 */
    {
        uint8_t p[2] = {0xC0, 0x00};
        MqttFixedHeader fh;
        T(MqttCodec_DecodeFixedHeader(p, 2, &fh) == MQTT_CODEC_OK &&
          fh.packet_type == MQTT_PKT_PINGREQ && fh.flags == 0U &&
          fh.remaining_length == 0U && fh.fixed_header_len == 2U, "PINGREQ fixed header");
    }
    /* PUBLISH QoS1 retain: 0x33 = type3<<4 | (qos1<<1)|retain1 = 0b00110011.
       RL=200 encodes as 0xC8 0x01 (200 = 72|0x80, carry 1). */
    {
        uint8_t p[4] = {0x33, 0xC8, 0x01, 0x00};
        MqttFixedHeader fh;
        T(MqttCodec_DecodeFixedHeader(p, 4, &fh) == MQTT_CODEC_OK &&
          fh.packet_type == MQTT_PKT_PUBLISH && fh.flags == 0x03U &&
          fh.remaining_length == 200U && fh.fixed_header_len == 3U, "PUBLISH QoS1+retain fixed header");
    }
    /* Oversized packet (> MQTT_MAX_PACKET_SIZE) at the packet boundary. */
    {
        /* RL=3000: byte0=0xB8(cont,56), byte1=0x17(23) -> 56+23*128=3000 > 2304. */
        uint8_t p[3] = {0x30, 0xB8, 0x17};
        MqttFixedHeader fh;
        T(MqttCodec_DecodeFixedHeader(p, 3, &fh) == MQTT_CODEC_PACKET_TOO_LARGE,
          "oversized PUBLISH RL -> PACKET_TOO_LARGE (no allocation)");
    }
    /* Reserved type 0 -> malformed. */
    {
        uint8_t p[2] = {0x00, 0x00};
        MqttFixedHeader fh;
        T(MqttCodec_DecodeFixedHeader(p, 2, &fh) == MQTT_CODEC_MALFORMED, "type0 -> MALFORMED");
    }
    /* Type 15 -> malformed. */
    {
        uint8_t p[2] = {0xF0, 0x00};
        MqttFixedHeader fh;
        T(MqttCodec_DecodeFixedHeader(p, 2, &fh) == MQTT_CODEC_MALFORMED, "type15 -> MALFORMED");
    }
}

static void test_connect(void)
{
    printf("\n== CONNECT ==\n");
    uint8_t buf[256];
    const char *cid = "room-sensor";
    size_t n = MqttCodec_EncodeConnect(buf, sizeof(buf), cid, strlen(cid), 60);
    T(n > 0U, "CONNECT encodes");
    /* decode fixed header: type1, flags0, RL = n-2 */
    MqttFixedHeader fh;
    T(MqttCodec_DecodeFixedHeader(buf, n, &fh) == MQTT_CODEC_OK &&
      fh.packet_type == MQTT_PKT_CONNECT && fh.flags == 0U &&
      fh.remaining_length == n - 2U, "CONNECT fixed header type1/flags0/RL");
    /* verify protocol name "MQTT" + level 4 + clean-session flag */
    size_t hdr = fh.fixed_header_len;
    T(buf[hdr+0]==0x00 && buf[hdr+1]==0x04 && !memcmp(buf+hdr+2,"MQTT",4), "protocol name MQTT");
    T(buf[hdr+6]==MQTT_PROTOCOL_LEVEL, "protocol level 4");
    T(buf[hdr+7]==0x02U, "connect flags clean session (0x02), no user/pass/will");
    /* keepalive 60 -> 0x00 0x3C */
    T(buf[hdr+8]==0x00 && buf[hdr+9]==0x3C, "keepalive 60");
    /* oversize client id rejected */
    {
        char longid[MQTT_MAX_CLIENT_ID_LENGTH+2];
        memset(longid, 'x', sizeof(longid)-1); longid[sizeof(longid)-1]='\0';
        size_t r = MqttCodec_EncodeConnect(buf, sizeof(buf), longid, strlen(longid), 60);
        T(r == 0U, "oversized client id -> rejected");
    }
}

static void test_publish(void)
{
    printf("\n== PUBLISH ==\n");
    uint8_t buf[256];
    const char *topic = "env/ota/room";
    uint8_t payload[32]; for (size_t i=0;i<sizeof(payload);i++) payload[i]=(uint8_t)i;

    size_t n0 = MqttCodec_EncodePublish(buf, sizeof(buf), topic, strlen(topic), payload,
                                        sizeof(payload), 0, false, 0);
    T(n0 > 0U, "QoS0 PUBLISH encodes");
    MqttFixedHeader fh;
    MqttCodec_DecodeFixedHeader(buf, n0, &fh);
    T(fh.packet_type==MQTT_PKT_PUBLISH && fh.flags==0U, "QoS0 flags=0");
    {
        MqttInboundPublish in;
        MqttCodecStatus rs = MqttCodec_DecodePublish(0, false, false, buf+fh.fixed_header_len,
                                                     fh.remaining_length, &in);
        T(rs==MQTT_CODEC_OK && in.topic_len==strlen(topic) &&
          !memcmp(in.topic, topic, strlen(topic)), "QoS0 decode topic");
        T(in.qos==0U && in.payload_len==sizeof(payload) &&
          !memcmp(in.payload, payload, sizeof(payload)), "QoS0 decode payload");
    }

    /* QoS1 with packet id. */
    size_t n1 = MqttCodec_EncodePublish(buf, sizeof(buf), topic, strlen(topic), payload,
                                        sizeof(payload), 1, true, 0x1234);
    T(n1 > 0U && n1 == n0 + 2U, "QoS1 PUBLISH adds 2 bytes (packet id)");
    MqttCodec_DecodeFixedHeader(buf, n1, &fh);
    T(fh.flags==0x03U, "QoS1+retain flags=0b0011");
    {
        MqttInboundPublish in;
        MqttCodecStatus rs = MqttCodec_DecodePublish(1, true, false, buf+fh.fixed_header_len,
                                                     fh.remaining_length, &in);
        T(rs==MQTT_CODEC_OK && in.packet_id==0x1234U && in.qos==1U && in.retain,
          "QoS1 decode packet id + retain");
    }
    /* QoS==3 rejected by encoder. */
    T(MqttCodec_EncodePublish(buf, sizeof(buf), topic, strlen(topic), payload,
                              sizeof(payload), 3, false, 0) == 0U, "QoS=3 PUBLISH rejected");
    /* Huge payload rejected. */
    {
        uint8_t big[MQTT_MAX_PAYLOAD_SIZE+4];
        T(MqttCodec_EncodePublish(buf, sizeof(buf), topic, strlen(topic), big, sizeof(big),
                                  0,false,0) == 0U, "payload > MQTT_MAX_PAYLOAD_SIZE rejected");
    }
}

static void test_suback_puback(void)
{
    printf("\n== SUBACK / PUBACK ==\n");
    /* PUBACK body = packet id 0x0010. */
    {
        uint8_t body[2] = {0x00, 0x10};
        MqttPuback pa;
        T(MqttCodec_DecodePuback(body, 2, &pa) == MQTT_CODEC_OK && pa.packet_id==0x0010U,
          "PUBACK decodes id");
        uint8_t bad[1] = {0x00};
        T(MqttCodec_DecodePuback(bad, 1, &pa) == MQTT_CODEC_MALFORMED, "PUBACK RL!=2 -> malformed");
    }
    /* SUBACK body = packet id + code 0x01. */
    {
        uint8_t body[3] = {0x00, 0x10, 0x01};
        MqttSuback sa;
        T(MqttCodec_DecodeSuback(body, 3, &sa) == MQTT_CODEC_OK && sa.packet_id==0x0010U &&
          sa.return_code==0x01U && sa.granted, "SUBACK granted (qos1)");
        uint8_t refuse[3] = {0x00, 0x10, 0x80};
        T(MqttCodec_DecodeSuback(refuse, 3, &sa) == MQTT_CODEC_OK && !sa.granted,
          "SUBACK 0x80 -> refused");
    }
}

int main(void)
{
    printf("Phase 17 MQTT codec tests\n");
    test_remaining_length();
    test_fixed_header();
    test_connect();
    test_publish();
    test_suback_puback();
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}