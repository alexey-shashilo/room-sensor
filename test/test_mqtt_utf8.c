#include <stdio.h>
#include <string.h>

#include "mqtt_codec.h"
#include "mqtt_client.h"
#include "mqtt_utf8.h"
#include "network_transport.h"
#include "fake_network_adapter.h"
#include "fake_platform_time.h"
#include "platform_time.h"

/* Phase 17.1 MQTT UTF-8 / topic / fixed-header compliance tests.

   1. UTF-8 validator byte-vector suite (ASCII, 2/3/4-byte, malformed, truncated,
      overlong, surrogate, >U+10FFFF, U+0000, U+FFFE/U+FFFF).
   2. Topic Name (PUBLISH) validation: valid multi-byte; rejects wildcards and
      malformed UTF-8.
   3. Topic Filter (SUBSCRIBE) validation: valid wildcard grammar; rejects bad
      placement.
   4. Client ID: valid; invalid UTF-8 rejected; byte-length prefix uses bytes.
   5. QoS0 + DUP=1 inbound PUBLISH -> protocol error (streaming).
   6. Fragmented malformed UTF-8 in a PUBLISH cannot reach the application
      callback. */

static int s_pass = 0, s_fail = 0, s_case = 0;
static void T(int cond, const char *name)
{
    s_case++;
    if (cond) { s_pass++; printf("  PASS #%d: %s\n", s_case, name); }
    else      { s_fail++; printf("  FAIL #%d: %s\n", s_case, name); }
}

static void test_utf8_valid(void)
{
    printf("\n== UTF-8 validator: valid sequences ==\n");
    /* ASCII. */
    T(MqttUtf8_Valid((const uint8_t*)"room/air/co2", 11), "ASCII valid");
    /* valid 2-byte: U+00E9 (C3 A9) */
    { uint8_t v[2]={0xC3,0xA9}; T(MqttUtf8_Valid(v,2), "valid 2-byte");
      /* Russian "температура" is > 64 bytes? "температура" ~ 11 chars *2 = 22 bytes, ok. */
      uint8_t ru[]={0xD1,0x82,0xD0,0xB5,0xD0,0xBC,0xD0,0xBF,0xD0,0xB5,0xD1,0x80,0xD0,0xB0,0xD1,0x82,0xD1,0x83,0xD1,0x80,0xD0,0xB0}; /* температура */
      T(MqttUtf8_Valid(ru,sizeof(ru)), "valid 2-byte (Cyrillic)");
    }
    /* valid 3-byte: U+20AC euro E2 82 AC */
    { uint8_t v[3]={0xE2,0x82,0xAC}; T(MqttUtf8_Valid(v,3), "valid 3-byte (euro)"); }
    /* valid 3-byte U+FFFD replacement char (not a noncharacter) EF BF BD */
    { uint8_t v[3]={0xEF,0xBF,0xBD}; T(MqttUtf8_Valid(v,3), "valid U+FFFD"); }
    /* valid 4-byte: U+1F600 emoji F0 9F 98 80 */
    { uint8_t v[4]={0xF0,0x9F,0x98,0x80}; T(MqttUtf8_Valid(v,4), "valid 4-byte (>U+FFFF)"); }
    /* valid at top U+10FFFF F4 8F BF BF */
    { uint8_t v[4]={0xF4,0x8F,0xBF,0xBF}; T(MqttUtf8_Valid(v,4), "valid U+10FFFF"); }
}

static void test_utf8_invalid(void)
{
    printf("\n== UTF-8 validator: invalid sequences ==\n");
    /* U+0000. */
    { uint8_t v[2]={0x00,0x41}; T(!MqttUtf8_Valid(v,2), "U+0000 rejected"); }
    /* lone continuation. */
    { uint8_t v[2]={0x80,0x41}; T(!MqttUtf8_Valid(v,2), "lone continuation rejected"); }
    /* truncated 2-byte. */
    { uint8_t v[1]={0xC3}; T(!MqttUtf8_Valid(v,1), "truncated 2-byte rejected"); }
    /* truncated 3-byte. */
    { uint8_t v[2]={0xE2,0x82}; T(!MqttUtf8_Valid(v,2), "truncated 3-byte rejected"); }
    /* truncated 4-byte. */
    { uint8_t v[3]={0xF0,0x9F,0x98}; T(!MqttUtf8_Valid(v,3), "truncated 4-byte rejected"); }
    /* overlong: C0 AF (overlong '/'). */
    { uint8_t v[2]={0xC0,0xAF}; T(!MqttUtf8_Valid(v,2), "overlong (C0) rejected"); }
    { uint8_t v[2]={0xC1,0x81}; T(!MqttUtf8_Valid(v,2), "overlong (C1) rejected"); }
    /* overlong 3-byte: E0 80 AF. */
    { uint8_t v[3]={0xE0,0x80,0xAF}; T(!MqttUtf8_Valid(v,3), "overlong 3-byte rejected"); }
    /* surrogate: ED A0 80 (U+D800). */
    { uint8_t v[3]={0xED,0xA0,0x80}; T(!MqttUtf8_Valid(v,3), "surrogate U+D800 rejected"); }
    { uint8_t v[3]={0xED,0xBF,0xBF}; T(!MqttUtf8_Valid(v,3), "surrogate U+DFFF rejected"); }
    /* > U+10FFFF: F4 90 80 80. */
    { uint8_t v[4]={0xF4,0x90,0x80,0x80}; T(!MqttUtf8_Valid(v,4), "> U+10FFFF rejected"); }
    /* noncharacters U+FFFE / U+FFFF. */
    { uint8_t v[3]={0xEF,0xBF,0xBE}; T(!MqttUtf8_Valid(v,3), "U+FFFE rejected"); }
    { uint8_t v[3]={0xEF,0xBF,0xBF}; T(!MqttUtf8_Valid(v,3), "U+FFFF rejected"); }
    /* invalid leader F5. */
    { uint8_t v[1]={0xF5}; T(!MqttUtf8_Valid(v,1), "F5 leader rejected"); }
    /* multi-sequence with one bad byte in the middle. */
    { uint8_t v[]={'a','b',0xC3,0x28,'c'}; T(!MqttUtf8_Valid(v,sizeof v), "bad continuation mid-string rejected"); }
    /* empty string rejected. */
    T(!MqttUtf8_Valid((const uint8_t*)"",0), "empty rejected");
}

static void test_topic_name(void)
{
    printf("\n== Topic Name validation ==\n");
    /* valid. */
    T(MqttUtf8_IsValidTopicName((const uint8_t*)"room/air/co2", 11), "valid ASCII topic");
    { uint8_t ru[]={0xD1,0x82,0xD0,0xB5,0xD0,0xBC,0xD0,0xBF,0xD0,0xB5,0xD1,0x80,0xD0,0xB0,0xD1,0x82,0xD1,0x83,0xD1,0x80,0xD0,0xB0};
      T(MqttUtf8_IsValidTopicName(ru,sizeof ru), "valid multi-byte Cyrillic topic (byte length)"); }
    T(MqttUtf8_IsValidTopicName((const uint8_t*)"/finance", 8), "leading slash valid");
    T(MqttUtf8_IsValidTopicName((const uint8_t*)"/", 1), "topic '/' valid");

    /* wildcards + malformed rejected. */
    T(!MqttUtf8_IsValidTopicName((const uint8_t*)"",0), "empty topic rejected");
    T(!MqttUtf8_IsValidTopicName((const uint8_t*)"room/#",6), "topic '#' rejected");
    T(!MqttUtf8_IsValidTopicName((const uint8_t*)"room/+",6), "topic '+' rejected");
    T(!MqttUtf8_IsValidTopicName((const uint8_t*)"room/#/x",8), "topic embedded # rejected");
    { uint8_t v[]={'a',0x00,'b'}; T(!MqttUtf8_IsValidTopicName(v,sizeof v), "embedded U+0000 rejected"); }
    { uint8_t v[2]={0xC3}; T(!MqttUtf8_IsValidTopicName(v,2), "truncated UTF-8 topic rejected"); }
    { uint8_t v[3]={0xED,0xA0,0x80}; T(!MqttUtf8_IsValidTopicName(v,3), "surrogate topic rejected"); }
    { uint8_t v[2]={0xC0,0xAF}; T(!MqttUtf8_IsValidTopicName(v,2), "overlong topic rejected"); }
}

static void test_topic_filter(void)
{
    printf("\n== Topic Filter validation ==\n");
    /* valid. */
    T(MqttUtf8_IsValidTopicFilter((const uint8_t*)"#",1), "# valid");
    T(MqttUtf8_IsValidTopicFilter((const uint8_t*)"sport/#",7), "sport/# valid");
    T(MqttUtf8_IsValidTopicFilter((const uint8_t*)"sport/+/player1",15), "+ mid valid");
    T(MqttUtf8_IsValidTopicFilter((const uint8_t*)"+/temperature",13), "+ first valid");
    T(MqttUtf8_IsValidTopicFilter((const uint8_t*)"+",1), "+ alone valid");
    T(MqttUtf8_IsValidTopicFilter((const uint8_t*)"/+",2), "/+ valid");

    /* invalid placement. */
    T(!MqttUtf8_IsValidTopicFilter((const uint8_t*)"sport/tennis#",13), "sport/tennis# invalid");
    T(!MqttUtf8_IsValidTopicFilter((const uint8_t*)"sport/#/ranking",15), "sport/#/ranking invalid");
    T(!MqttUtf8_IsValidTopicFilter((const uint8_t*)"sport+",6), "sport+ invalid");
    T(!MqttUtf8_IsValidTopicFilter((const uint8_t*)"foo/+bar",8), "foo/+bar invalid");
    T(!MqttUtf8_IsValidTopicFilter((const uint8_t*)"foo/bar#baz",11), "foo/bar#baz invalid");
    T(!MqttUtf8_IsValidTopicFilter((const uint8_t*)"",0), "empty filter invalid");
}

static void test_client_id(void)
{
    printf("\n== Client ID validation ==\n");
    /* valid UTF-8 client id (multi-byte, byte-length prefix). */
    { uint8_t id[]={0xD0,0x9A,0xD0,0xB8};  /* "Ки" 4 bytes */
      T(MqttUtf8_Valid(id,sizeof id), "multi-byte client id valid UTF-8"); }
    /* invalid UTF-8 client id rejected. */
    { uint8_t id[2]={0xC3}; T(!MqttUtf8_Valid(id,1), "truncated client id rejected"); }
}

static void test_byte_length(void)
{
    printf("\n== UTF-8 encoded length uses bytes ==\n");
    /* "éx" : C3 A9 78 -> 3 bytes. */
    uint8_t topic[3]={0xC3,0xA9,0x78};
    T(MqttUtf8_IsValidTopicName(topic,3), "3-byte UTF-8 topic valid");
    /* encoded prefix must equal byte length, not code points. The QoS0 PUBLISH
       with topic "éx" must carry topic-length == 3. */
    uint8_t pkt[MQTT_MAX_PACKET_SIZE];
    size_t n = MqttCodec_EncodePublish(pkt, sizeof(pkt), (const char*)topic, 3,
                                       (const uint8_t*)"p", 1, 0, false, 0);
    T(n > 0U, "publish with multi-byte topic encodes");
    T(n >= 7U && pkt[2]==0x00 && pkt[3]==0x03, "topic-length prefix == UTF-8 BYTE length (3)");
}

/* --- streaming client tests (QoS0+DUP=1 and fragmented malformed UTF-8) --- */
static FakeNetworkAdapter      s_fake;
static NetworkTransportAdapter s_adapter;
static NetworkTransport        s_t;
static MqttClient              s_mc;
static bool                    s_cb_called;

static bool on_publish(void *ctx, const MqttInboundPublish *pub)
{
    (void)ctx; (void)pub;
    s_cb_called = true;
    return true;
}

static void client_setup(void)
{
    s_cb_called = false;
    FakePlatform_SetTick(0);
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    NetworkEndpoint ep;
    memset(&ep,0,sizeof(ep));
    ep.port=1883; memcpy(ep.host,"broker.example",14);
    NetworkTransport_Init(&s_t,&s_adapter,&ep);
    MqttConnectConfig cfg;
    memset(&cfg,0,sizeof(cfg));
    cfg.client_id="u"; cfg.client_id_len=1; cfg.keepalive_s=0;
    MqttClient_Init(&s_mc,&s_t,&cfg,on_publish,NULL);
}

static void client_connect(void)
{
    MqttClient_Connect(&s_mc);
    int g=0;
    while (MqttClient_GetState(&s_mc)==MQTT_STATE_CONNECTING_TRANSPORT && g<60)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    uint8_t ca[4]={0x20,0x02,0x00,0x00};
    FakeNetworkAdapter_FeedRecv(&s_fake,ca,4);
    while (MqttClient_GetState(&s_mc)!=MQTT_STATE_CONNECTED && g<120)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
}

static void test_qos0_dup1(void)
{
    printf("\n== QoS0 + DUP=1 inbound PUBLISH ==\n");
    client_setup();
    client_connect();
    /* PUBLISH QoS0 retain=0 DUP=1: byte0 = 0x30 | 0x08 = 0x38. RL=4
       (2 topiclen + 1 topic 'a' + 1 payload 'x'). */
    uint8_t p2[6]={0x38, 0x04, 0x00, 0x01, 'a', 0x78};
    FakeNetworkAdapter_FeedRecv(&s_fake,p2,6);
    int g=0;
    while (MqttClient_GetState(&s_mc)!=MQTT_STATE_ERROR && g<30)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    T(MqttClient_GetState(&s_mc)==MQTT_STATE_ERROR, "QoS0 + DUP=1 -> protocol error (ERROR)");
    T(!s_cb_called, "no callback for malformed QoS0+DUP=1 publish");
}

static void test_fragmented_malformed_topic(void)
{
    printf("\n== fragmented malformed UTF-8 topic cannot reach callback ==\n");
    client_setup();
    client_connect();
    /* PUBLISH QoS0 topic = lone 0xC2 (truncated 2-byte seq). Packet:
       0x30, RL=2+1+1=4, topiclen 0x0001, 0xC2, 'x'. */
    uint8_t frag[6]={0x30, 0x04, 0x00, 0x01, 0xC2, 0x78};
    /* feed byte-by-byte to fragment across Receive boundaries. */
    int g=0;
    for (int i=0;i<6 && MqttClient_GetState(&s_mc)!=MQTT_STATE_ERROR;i++)
    {
        FakeNetworkAdapter_FeedRecv(&s_fake,frag+i,1);
        FakePlatform_AdvanceTick(1);
        MqttClient_Run(&s_mc);
        g++;
    }
    T(MqttClient_GetState(&s_mc)==MQTT_STATE_ERROR, "fragmented malformed-UTF8 topic -> ERROR");
    T(!s_cb_called, "INVALID_UTF8_CAN_REACH_APPLICATION_CALLBACK = NO");
    /* also confirm QoS0 outbound DUP=0: run an outbound publish and inspect its
       fixed header byte. */
    (void)g;
}

static void test_qos0_outbound_dup0(void)
{
    printf("\n== QoS0 outbound DUP=0 ==\n");
    client_setup();
    client_connect();
    size_t pre=s_fake.tx_capture_len;
    MqttClient_PublishQos0(&s_mc,"a",1,(const uint8_t*)"x",1,false);
    int g=0;
    while (s_fake.tx_capture_len==pre && g<60)
    { FakePlatform_AdvanceTick(1); MqttClient_Run(&s_mc); g++; }
    T(s_fake.tx_capture_len>=pre+1U && (s_fake.tx_capture[pre] & 0x08U)==0U &&
      ((s_fake.tx_capture[pre]>>1)&0x03U)==0U, "QoS0 outbound fixed header DUP=0, QoS=0");
}

int main(void)
{
    printf("Phase 17.1 MQTT UTF-8 / topic / fixed-header compliance tests\n");
    test_utf8_valid();
    test_utf8_invalid();
    test_topic_name();
    test_topic_filter();
    test_client_id();
    test_byte_length();
    test_qos0_dup1();
    test_fragmented_malformed_topic();
    test_qos0_outbound_dup0();
    printf("\n=== Summary ===\n");
    printf("  Cases: %d\n", s_case);
    printf("  Passed: %d\n", s_pass);
    printf("  Failed: %d\n", s_fail);
    return s_fail > 0 ? 1 : 0;
}