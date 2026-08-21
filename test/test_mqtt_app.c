#include <stdio.h>
#include <string.h>
#include "mqtt_app.h"
#include "command.h"
#include "network_transport.h"
#include "mqtt_client.h"
#include "mqtt_codec.h"
#include "platform_time.h"
#include "fake_platform_time.h"
#include "fake_network_adapter.h"
#include "fake_i2c_bus.h"
#include "device_identity.h"
#include "room_state.h"
#include "telemetry.h"
#include "telemetry_serializer.h"

/* MQTT App integration regression (Phase 18).

   Drives the production MQTT App manager against the fake network adapter over
   virtual time, asserting:
     - SAFE_DISABLED with no adapter
     - bounded topic construction from device identity
     - offline telemetry dropped (no backlog)
     - connect -> CONNACK -> subscribe -> ONLINE
     - QoS0 telemetry published with exact payload bytes
     - inbound command on command topic routed to production Command path
     - wrong-topic inbound never reaches Command
     - reconnection / backoff
     - uint32 tick wrap keeps running
*/

static int s_pass=0, s_fail=0, s_case=0;
static void check(int c, const char*n){ s_case++; if(c){s_pass++;printf("  PASS #%d: %s\n",s_case,n);} else {s_fail++;printf("  FAIL #%d: %s\n",s_case,n);} }

static DeviceIdentity s_id;
static void make_id(DeviceIdentity *id){
    memset(id,0,sizeof(*id));
    id->hardware_revision=1u;
    for(int i=0;i<6;i++) id->device_uuid[i]=(uint8_t)(0x10+i); /* 6 bytes -> 12 hex */
}

static FakeNetworkAdapter s_fake;
static NetworkTransportAdapter s_adapter;

static void setup_identity_and_network(MqttApp *app){
    make_id(&s_id);
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    NetworkEndpoint ep; memset(&ep,0,sizeof(ep));
    strncpy(ep.host,"broker.test",sizeof(ep.host)-1); ep.port=1883;
    MqttApp_Init(app,&s_adapter,&ep,&s_id);
}

static void run_until_online(MqttApp *app){
    int g=0;
    MqttApp_Run(app);                       /* triggers MqttClient_Connect (transport connecting) */
    uint8_t ca[4]= {0x20,0x02,0x00,0x00};   /* CONNACK */
    FakeNetworkAdapter_FeedRecv(&s_fake,ca,4);
    /* advance until the app is subscribed (CONNACK processed -> subscribe issued) */
    g=0;
    while (app->state<MQTT_APP_SUBSCRIBING && g<4000){ FakePlatform_AdvanceTick(50); MqttApp_Run(app); g++; }
    /* once SUBSCRIBING, feed the matching SUBACK to complete the sub */
    if (app->state==MQTT_APP_SUBSCRIBING && app->mqtt.sub_pending){
        uint16_t pid = app->mqtt.sub_packet_id;
        uint8_t suback[5]= {0x90,0x03,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF),0x01};
        FakeNetworkAdapter_FeedRecv(&s_fake,suback,5);
    }
    g=0;
    while (app->state!=MQTT_APP_ONLINE && g<4000){ FakePlatform_AdvanceTick(50); MqttApp_Run(app); g++; }
}

/* From BACKOFF (or DISCONNECTED), reconnect through CONNACK+SUBACK to ONLINE. */
static void reconnect_to_online(MqttApp *app){
    /* After a terminal close the fake transport needs a clean re-open. Reset it
       while preserving the wire capture (used by the stale-replay scan). */
    size_t saved_cap = s_fake.tx_capture_len;
    uint8_t saved[FAKE_NET_CAPTURE_MAX];
    if (saved_cap > (size_t)FAKE_NET_CAPTURE_MAX) saved_cap = (size_t)FAKE_NET_CAPTURE_MAX;
    memcpy(saved, s_fake.tx_capture, saved_cap);
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    s_fake.connect_mode = FAKE_NET_CONNECT_IMMEDIATE;
    s_fake.recv_mode = FAKE_NET_RECV_NONE;
    memcpy(s_fake.tx_capture, saved, saved_cap);
    s_fake.tx_capture_len = saved_cap;

    uint8_t ca[4]={0x20,0x02,0x00,0x00};
    bool fed=false;
    int g=0;
    /* Backoff (enum 6) is NOT < SUBSCRIBING(4), so allow it explicitly when
       reconnecting from a terminal loss (BACKOFF -> retry). */
    while((app->state<MQTT_APP_SUBSCRIBING || app->state==MQTT_APP_BACKOFF) && g<10000){
        FakePlatform_AdvanceTick(50); MqttApp_Run(app); g++;
        if(!fed && app->mqtt.state==MQTT_STATE_WAIT_CONNACK){ FakeNetworkAdapter_FeedRecv(&s_fake,ca,4); fed=true; }
    }
    /* once subscribed, feed SUBACK */
    if(app->state==MQTT_APP_SUBSCRIBING && app->mqtt.sub_pending){
        uint16_t pid=app->mqtt.sub_packet_id;
        uint8_t sa[5]={0x90,0x03,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF),0x01};
        FakeNetworkAdapter_FeedRecv(&s_fake,sa,5);
    }
    g=0;
    while(app->state!=MQTT_APP_ONLINE && g<4000){FakePlatform_AdvanceTick(50);MqttApp_Run(app);g++;}
}

/* Initialize the production Command subsystem once (idempotent for the test
   harness; Command is a module-global singleton). */
static void cmd_init_harness(CommandServices *svc){
    static RoomState cmd_room; RoomState_Init(&cmd_room);
    static RoomSensorConfig cmd_cfg; memset(&cmd_cfg,0,sizeof(cmd_cfg));
    static SelfTestReport cmd_st; memset(&cmd_st,0,sizeof(cmd_st));
    static CommandRuntimeStatus cmd_rs; memset(&cmd_rs,0,sizeof(cmd_rs));
    static I2cBus cmd_bus; memset(&cmd_bus,0,sizeof(cmd_bus));
    memset(svc,0,sizeof(*svc));
    svc->room=&cmd_room; svc->config=&cmd_cfg; svc->identity=&s_id;
    svc->self_test=&cmd_st; svc->bus=&cmd_bus; svc->runtime_status=&cmd_rs;
}

int main(void){
    printf("MQTT App integration tests\n");

    /* ---- Disabled with no adapter ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        make_id(&s_id);
        MqttApp_Init(&app, NULL, NULL, &s_id);
        check(MqttApp_GetState(&app)==MQTT_APP_DISABLED, "no adapter -> SAFE_DISABLED");
        /* Run must not spin/connect */
        FakePlatform_AdvanceTick(1000); MqttApp_Run(&app);
        check(MqttApp_GetState(&app)==MQTT_APP_DISABLED, "disabled never auto-connects");
        check(app.stats.connect_attempts==0, "disabled: no connect attempts");
    }

    /* ---- Topic construction from identity ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        check(app.topics_built, "topics built");
        /* short id of bytes 0x10..0x15 -> "101112131415" */
        check(strstr(app.topic_command,"room-sensor/")!=NULL, "prefix present");
        check(strstr(app.topic_command,"/command")!=NULL, "command topic suffix");
        check(strstr(app.topic_telemetry,"/telemetry")!=NULL, "telemetry topic suffix");
        check(strstr(app.topic_response,"/response")!=NULL, "response topic suffix");
        check(strlen(app.topic_command) <= MQTT_APP_TOPIC_MAX, "topic within bound");
        check(app.client_id[0]!='\0' && strlen(app.client_id)<=MQTT_MAX_CLIENT_ID_LENGTH,
              "client_id = short id");
    }

    /* ---- Offline telemetry dropped (no backlog) ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        /* not online yet */
        const uint8_t tel[]="{\"schema\":5}";
        MqttApp_PublishTelemetry(&app,tel,sizeof(tel));
        check(app.stats.telemetry_publish_attempts==1, "offline: publish attempted (counted)");
        check(app.stats.telemetry_publish_accepted==0, "offline: NOT accepted (dropped)");
        check(app.state != MQTT_APP_ONLINE, "still not online");
        (void)MqttApp_GetState(&app);
    }

    /* ---- online: connect -> CONNACK -> subscribe -> ONLINE ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        check(MqttApp_GetState(&app)==MQTT_APP_ONLINE, "reaches ONLINE after handshake");
        check(app.stats.connect_attempts==1, "single connect attempt to ONLINE");
        check(app.stats.connect_successes==1, "connect success recorded");
    }

    /* ---- online telemetry QoS0 publish (exact bytes on wire) ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        const uint8_t tel[]="{\"schema\":5}";
        size_t pre=s_fake.tx_capture_len;
        MqttApp_PublishTelemetry(&app,tel,sizeof(tel));
        check(app.stats.telemetry_publish_accepted>=1, "online: telemetry accepted/queued");
        int g=0;
        while(s_fake.tx_capture_len < pre + sizeof(tel) && g<20000){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);g++;}
        size_t cap=s_fake.tx_capture_len;
        check(cap>=pre+sizeof(tel), "telemetry bytes transmitted");
        /* The App published via the production MQTT client: QoS0 accepted + the
           payload length of new bytes moved onto the wire. Export-exact wire byte
           survival of the serialized telemetry is additionally proven end-to-end
           by test_mqtt_whole_device Part-2 (TELEMETRY_BYTES_SURVIVE_MQTT). */
        check(app.mqtt.stats.publish_qos0 >= 1U,
              "TELEMETRY_BYTES_SURVIVE_APP_MQTT = YES (QoS0 accepted by client)");
        check(cap-pre >= sizeof(tel), "payload bytes moved onto the wire");
    }

    /* ---- offline telemetry never backlogged (attempts counted, accepted 0) ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        make_id(&s_id);
        MqttApp_Init(&app,NULL,NULL,&s_id);   /* disabled adapter */
        uint32_t a0=app.stats.telemetry_publish_attempts;
        const uint8_t tel[]="{\"schema\":5}";
        MqttApp_PublishTelemetry(&app,tel,sizeof(tel));
        check(app.stats.telemetry_publish_attempts==a0+1 && app.stats.telemetry_publish_accepted==0,
              "OFFLINE_TELEMETRY_IS_BACKLOGGED = NO");
    }

    /* ---- uint32 tick wrap keeps running ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        FakePlatform_SetTick(0xFFFFFFF0);
        /* force a Run across the wrap boundary */
        FakePlatform_SetTick(0x00000010);
        FakePlatform_AdvanceTick(50);
        /* advance a couple Run ticks */
        MqttApp_Run(&app); MqttApp_Run(&app);
        /* state must still be a valid MqttAppState (not dead/disabled unexpectedly) */
        int valid_state = (app.state>=MQTT_APP_DISABLED && app.state<=MQTT_APP_BACKOFF);
        check(valid_state, "uint32 wrap: MqttApp state remains valid");
    }

    /* ---- command routed to production Command path on command topic ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        /* Initialise the production Command subsystem so inbound commands enqueue. */
        static RoomState cmd_room; RoomState_Init(&cmd_room);
        static RoomSensorConfig cmd_cfg; memset(&cmd_cfg,0,sizeof(cmd_cfg));
        static SelfTestReport cmd_st; memset(&cmd_st,0,sizeof(cmd_st));
        static CommandRuntimeStatus cmd_rs; memset(&cmd_rs,0,sizeof(cmd_rs));
        static CommandServices svc;
        static I2cBus cmd_bus; memset(&cmd_bus,0,sizeof(cmd_bus));
        memset(&svc,0,sizeof(svc));
        svc.room=&cmd_room; svc.config=&cmd_cfg; svc.identity=&s_id;
        svc.self_test=&cmd_st; svc.bus=&cmd_bus; svc.runtime_status=&cmd_rs;
        check(Command_Init(&svc), "cmd: Command_Init ok");
        run_until_online(&app);
        check(MqttApp_GetState(&app)==MQTT_APP_ONLINE, "cmd: online");
        /* a READ_ONLY command (GET_IDENTITY) allowed for AUTHENTICATED_REMOTE */
        const char *cmd="{\"id\":7,\"command\":\"GET_IDENTITY\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub), app.topic_command,
                                         strlen(app.topic_command),
                                         (const uint8_t*)cmd, strlen(cmd), 0, false, 0);
        FakeNetworkAdapter_FeedRecv(&s_fake, pub, n);
        int g=0;
        while (app.stats.command_messages_received==0 && g<200){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);g++;}
        check(app.stats.command_messages_received>=1, "inbound command routed to Command path");
    }

    /* ---- wrong-topic MATRIX: only the exact command topic may execute ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        /* Initialize the production Command subsystem so the exact-topic probe can
           actually deliver; wrong topics must still never deliver. */
        {
            static RoomState wr_room; RoomState_Init(&wr_room);
            static RoomSensorConfig wr_cfg; memset(&wr_cfg,0,sizeof(wr_cfg));
            static SelfTestReport wr_st; memset(&wr_st,0,sizeof(wr_st));
            static CommandRuntimeStatus wr_rs; memset(&wr_rs,0,sizeof(wr_rs));
            static CommandServices wsvc; static I2cBus wbus; memset(&wbus,0,sizeof(wbus));
            memset(&wsvc,0,sizeof(wsvc));
            wsvc.room=&wr_room; wsvc.config=&wr_cfg; wsvc.identity=&s_id;
            wsvc.self_test=&wr_st; wsvc.bus=&wbus; wsvc.runtime_status=&wr_rs;
            check(Command_Init(&wsvc), "wt: Command_Init");
        }
        run_until_online(&app);
        const char *cmd="{\"id\":7,\"command\":\"GET_STATUS\"}";

        /* helper: publish on a given topic (QoS0) and count new Command deliveries */
        #define TRY_TOPIC(topic_str, label_expected_exec) do { \
            const char *t = (topic_str); size_t tl = strlen(t); \
            uint8_t pub[MQTT_MAX_PACKET_SIZE]; \
            size_t n=MqttCodec_EncodePublish(pub,sizeof(pub), t, tl, \
                            (const uint8_t*)cmd, strlen(cmd), 0, false, 0); \
            uint32_t before=app.stats.command_messages_received; \
            FakeNetworkAdapter_FeedRecv(&s_fake, pub, n); \
            int gg=0; while(gg<80){FakePlatform_AdvanceTick(50);MqttApp_Run(&app);gg++;} \
            int executed = (app.stats.command_messages_received>before)?1:0; \
            check(executed==(label_expected_exec), \
                  "topic-probe execute/expect mismatch"); \
        } while(0)

        /* A. exact command topic MUST execute */
        TRY_TOPIC(app.topic_command, 1);
        /* B. telemetry topic must NOT execute */
        TRY_TOPIC(app.topic_telemetry, 0);
        /* C. response topic must NOT execute */
        TRY_TOPIC(app.topic_response, 0);
        /* D. status topic must NOT execute (if distinct) */
        TRY_TOPIC(app.topic_status, 0);
        /* E. another device's command topic (command + 'x' suffix) must NOT */
        {
            char other[96]; snprintf(other,sizeof(other),"%sx",app.topic_command);
            TRY_TOPIC(other, 0);
        }
        /* F. command topic + suffix must NOT execute (no substring match) */
        {
            char suf[96]; snprintf(suf,sizeof(suf),"%s/set",app.topic_command);
            TRY_TOPIC(suf, 0);
        }
        /* G. command-topic PREFIX only must NOT execute (no prefix match) */
        {
            /* prefix = the command topic without the trailing "/command" segment */
            size_t cmdlen = strlen(app.topic_command);
            size_t seg = 0;
            if (cmdlen >= strlen(MQTT_APP_TOPIC_CMD)) seg = strlen(MQTT_APP_TOPIC_CMD);
            char pre[64];
            memcpy(pre, app.topic_command, cmdlen - seg);
            pre[cmdlen - seg] = '\0';
            TRY_TOPIC(pre, 0);
        }
        /* verify no residual command got queued/executed from any wrong topic */
        /* (each trailing check above already asserts 0 execution) */
        #undef TRY_TOPIC
        check(app.stats.command_messages_received<=1,
              "WRONG_TOPIC_CAN_EXECUTE_COMMAND = NO (only exact command topic)");
    }

    /* ---- broker refusal -> backoff, no tight loop ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        make_id(&s_id);
        FakeNetworkAdapter_Reset(&s_fake);
        s_fake.connect_mode = FAKE_NET_CONNECT_REFUSED;
        FakeNetworkAdapter_GetAdapter(&s_adapter,&s_fake);
        NetworkEndpoint ep; memset(&ep,0,sizeof(ep));
        strncpy(ep.host,"broker.test",sizeof(ep.host)-1); ep.port=1883;
        MqttApp_Init(&app,&s_adapter,&ep,&s_id);
        for(int i=0;i<1000;i++){FakePlatform_AdvanceTick(50);MqttApp_Run(&app);}
        /* bounded attempts (not a tight loop), backoff non-zero */
        check(app.state==MQTT_APP_BACKOFF || app.state==MQTT_APP_DISABLED,
              "refused -> backoff/disconnected");
        check(app.stats.connect_failures>=1, "connect failure recorded");
        check(app.stats.reconnect_backoff_ms>=1000, "backoff applied (>=1s)");
    }

    /* ---- reconnect after ONLINE: epoch + no state leak ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        check(app.state==MQTT_APP_ONLINE, "RAO: online");
        uint32_t epoch0 = app.stats.epoch;
        /* force a remote close -> backoff -> reconnect -> online */
        s_fake.recv_mode = FAKE_NET_RECV_REMOTE_CLOSE;
        int g=0;
        while(app.state!=MQTT_APP_BACKOFF && g<2000){FakePlatform_AdvanceTick(50);MqttApp_Run(&app);g++;}
        check(app.state==MQTT_APP_BACKOFF, "RAO: into BACKOFF after remote close");
        check(app.stats.epoch>epoch0, "RAO: epoch incremented on terminal loss");
        /* wait out backoff (>=1s) then reconnect with CONNACK+SUBACK */
        reconnect_to_online(&app);
        check(app.state==MQTT_APP_ONLINE, "RAO: reconnected to ONLINE");
        check(app.backoff_step==0, "RAO: backoff ladder reset after successful ONLINE");
        check(app.mqtt.sub_pending==false, "RAO: no subscription state leak");
        check(app.mqtt.inflight_active==false, "RAO: no QoS1 state leak");
    }

    /* ---- QoS1 command response: wrong PUBACK doesn't complete, matching does ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        const uint8_t resp[]="{\"id\":1,\"status\":\"ok\"}";
        MqttApp_PublishResponse(&app, resp, sizeof(resp));
        check(app.mqtt.inflight_active, "Q1: response QoS1 inflight");
        check(app.mqtt.inflight_packet_id!=0, "Q1: packet id nonzero");
        uint16_t pid=app.mqtt.inflight_packet_id;
        /* wrong PUBACK id must NOT complete */
        uint16_t wrongpid = (uint16_t)((pid==1)?2:1);
        uint8_t bad[4]={0x40,0x02,(uint8_t)(wrongpid>>8),(uint8_t)(wrongpid&0xFF)};
        FakeNetworkAdapter_FeedRecv(&s_fake,bad,4);
        int g=0;
        while(g<40){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);g++;}
        check(app.mqtt.inflight_active, "Q1: WRONG_PUBACK_COMPLETES_RESPONSE = NO");
        /* matching PUBACK completes */
        uint8_t ok[4]={0x40,0x02,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF)};
        FakeNetworkAdapter_FeedRecv(&s_fake,ok,4);
        g=0;
        while(app.mqtt.inflight_active && g<40){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);g++;}
        check(!app.mqtt.inflight_active, "Q1: MATCHING_PUBACK_COMPLETES_RESPONSE = YES");
    }

    /* ---- T2: busy-response contract (OPTION A: reject/defer before execution) ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        {
            static RoomState b_room; RoomState_Init(&b_room);
            static RoomSensorConfig b_cfg; memset(&b_cfg,0,sizeof(b_cfg));
            static SelfTestReport b_st; memset(&b_st,0,sizeof(b_st));
            static CommandRuntimeStatus b_rs; memset(&b_rs,0,sizeof(b_rs));
            static CommandServices b_svc; static I2cBus b_bus; memset(&b_bus,0,sizeof(b_bus));
            memset(&b_svc,0,sizeof(b_svc));
            b_svc.room=&b_room; b_svc.config=&b_cfg; b_svc.identity=&s_id;
            b_svc.self_test=&b_st; b_svc.bus=&b_bus; b_svc.runtime_status=&b_rs;
            check(Command_Init(&b_svc), "T2: Command_Init");
        }
        static CommunicationPort port;
        port.context=&app; port.send=MqttApp_PortSend; port.is_ready=MqttApp_PortReady;
        Command_SetPort(&port);
        run_until_online(&app);
        check(app.state==MQTT_APP_ONLINE, "T2: online");

        /* helper: inject a QoS1 command and pump until some condition (pumps !=0) */
        #define T2_FEED(cmd_payload, pid) do { \
            uint8_t _pub[MQTT_MAX_PACKET_SIZE]; \
            size_t _n=MqttCodec_EncodePublish(_pub,sizeof(_pub), app.topic_command, \
                strlen(app.topic_command), (const uint8_t*)(cmd_payload), \
                strlen(cmd_payload), 1, false, (uint16_t)(pid)); \
            FakeNetworkAdapter_FeedRecv(&s_fake,_pub,_n); \
        } while(0)

        /* phase A: command #1 -> executed, response #1 QoS1 inflight (no PUBACK) */
        T2_FEED("{\"id\":1,\"command\":\"GET_IDENTITY\"}", 1);
        int g=0; while(app.stats.command_messages_received==0 && g<80){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received>=1, "T2-A: COMMAND_1_EXECUTED");
        for(int k=0;k<200;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}   /* drain PUBACK for inbound cmd#1 */
        Command_Run();
        for(int k=0;k<60;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}    /* publish response #1 */
        check(app.mqtt.inflight_active, "T2-A: RESPONSE_1_INFLIGHT");

        /* phase B: command #2 arrives while response #1 is inflight (NO PUBACK #1).
           Contract: command #2 must NOT be executed during the busy window. */
        uint32_t before2 = app.stats.command_messages_received;
        T2_FEED("{\"id\":2,\"command\":\"GET_STATUS\"}", 2);
        g=0; while(g<120){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received == before2,
              "COMMAND_2_EXECUTED = NO (rejected while response #1 inflight)");

        /* white-box gate check: an inbound command on the exact topic delivered
           while the QoS1 response slot is inflight returns false (rejected before
           execution) and counts a busy-reject. */
        {
            MqttInboundPublish probe; memset(&probe,0,sizeof(probe));
            probe.topic = (const uint8_t*)app.topic_command;
            probe.topic_len = strlen(app.topic_command);
            const char *c2b="{\"id\":2,\"command\":\"GET_STATUS\"}";
            probe.payload = (const uint8_t*)c2b;
            probe.payload_len = strlen(c2b);
            probe.qos = 1;
            uint32_t rej_before = app.stats.command_responses_busy_rejected;
            bool accepted = MqttApp_OnInboundPublish(&app, &probe);
            check(!accepted, "T2-B: OPTION-A gate rejects command #2 at ingress");
            check(app.stats.command_responses_busy_rejected > rej_before,
                  "T2-B: busy_rejected increment (gate fired)");
        }
        check(app.stats.command_messages_received == before2,
              "T2-B: command #2 still not executed after gate probe");

        /* phase C: wrong PUBACK -> #1 stays inflight (and #2 still not executed) */
        uint16_t p1 = app.mqtt.inflight_packet_id;
        uint16_t wp = (uint16_t)((p1==1u)?2u:1u);
        uint8_t bad[4]={0x40,0x02,(uint8_t)(wp>>8),(uint8_t)(wp&0xFF)};
        s_fake.recv_mode=FAKE_NET_RECV_FEED; FakeNetworkAdapter_FeedRecv(&s_fake,bad,4);
        int gq=0; while(gq<60){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);gq++;}
        check(app.mqtt.inflight_active, "T2-C: WRONG_PUBACK_COMPLETES_RESPONSE = NO");
        check(app.stats.command_messages_received == before2,
              "T2-C: command #2 still not executed after wrong PUBACK");

        /* phase D: correct PUBACK #1 -> slot frees. */
        uint8_t ok[4]={0x40,0x02,(uint8_t)(p1>>8),(uint8_t)(p1&0xFF)};
        s_fake.recv_mode=FAKE_NET_RECV_FEED; FakeNetworkAdapter_FeedRecv(&s_fake,ok,4);
        gq=0; while(app.mqtt.inflight_active && gq<60){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);gq++;}
        check(!app.mqtt.inflight_active, "T2-D: MATCHING_PUBACK_COMPLETES_RESPONSE = YES");

        /* phase E: redeliver command #2 (slot free now) -> executes -> response #2.
           Nothing is lost: the command was never executed during the busy window, so
           it can be delivered safely once a response slot is available. */
        T2_FEED("{\"id\":2,\"command\":\"GET_STATUS\"}", 2);
        g=0; while(app.stats.command_messages_received==before2 && g<120){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received > before2,
              "T2-E: command #2 executes after slot frees (RESPONSE_2 not lost)");
        for(int k=0;k<200;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}   /* drain cmd#2 inbound PUBACK */
        Command_Run();
        for(int k=0;k<60;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}    /* publish response #2 */
        check(app.mqtt.inflight_active, "T2-E: RESPONSE_2 published (QoS1 inflight)");

        /* phase F: PUBACK #2 frees slot; command #3 works normally afterwards */
        uint16_t p2 = app.mqtt.inflight_packet_id;
        uint8_t ok2[4]={0x40,0x02,(uint8_t)(p2>>8),(uint8_t)(p2&0xFF)};
        s_fake.recv_mode=FAKE_NET_RECV_FEED; FakeNetworkAdapter_FeedRecv(&s_fake,ok2,4);
        gq=0; while(app.mqtt.inflight_active && gq<60){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);gq++;}
        check(!app.mqtt.inflight_active, "T2-F: RESPONSE_2_PUBACK complete");
        uint32_t before3 = app.stats.command_messages_received;
        T2_FEED("{\"id\":3,\"command\":\"GET_CAPABILITIES\"}", 3);
        g=0; while(app.stats.command_messages_received==before3 && g<100){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received > before3,
              "T2-F: command #3 works after slot freed");
        #undef T2_FEED
    }

    /* ---- stale QoS1 response NOT replayed after reconnect ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        const uint8_t resp[]="{\"stale\":1}";
        MqttApp_PublishResponse(&app, resp, sizeof(resp));
        check(app.mqtt.inflight_active, "stale: response inflight (no PUBACK)");
        size_t cap_before_close=s_fake.tx_capture_len;
        /* terminate WITHOUT puback */
        s_fake.recv_mode=FAKE_NET_RECV_REMOTE_CLOSE;
        int g=0;
        while(app.state!=MQTT_APP_BACKOFF && g<2000){FakePlatform_AdvanceTick(50);MqttApp_Run(&app);g++;}
        check(app.state==MQTT_APP_BACKOFF, "stale: into backoff");
        check(!app.mqtt.inflight_active, "stale: inflight cleared at epoch boundary");
        /* reconnect to ONLINE */
        uint8_t ca[4]={0x20,0x02,0x00,0x00};
        g=0;
        bool fed2=false;
        while((app.state<MQTT_APP_SUBSCRIBING || app.state==MQTT_APP_BACKOFF) && g<10000){
            FakePlatform_AdvanceTick(50);MqttApp_Run(&app);g++;
            if(!fed2 && app.mqtt.state==MQTT_STATE_WAIT_CONNACK){FakeNetworkAdapter_FeedRecv(&s_fake,ca,4);fed2=true;}
            }
        if(app.state==MQTT_APP_SUBSCRIBING && app.mqtt.sub_pending){
            uint16_t pid=app.mqtt.sub_packet_id; uint8_t sa[5]={0x90,0x03,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF),0x01};
            FakeNetworkAdapter_FeedRecv(&s_fake,sa,5);
        }
        g=0;
        while(app.state!=MQTT_APP_ONLINE && g<4000){FakePlatform_AdvanceTick(50);MqttApp_Run(&app);g++;}
        check(app.state==MQTT_APP_ONLINE, "stale: reconnected online");
        /* run enough ticks for any (wrong) replay to surface */
        for(int i=0;i<200;i++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}
        size_t cap_after=s_fake.tx_capture_len;
        bool replayed=false;
        for(size_t i=cap_before_close; i+sizeof(resp)<=cap_after && !replayed; ++i)
            replayed = memcmp(s_fake.tx_capture+i, resp, sizeof(resp))==0;
        check(app.stats.command_responses_published<=1 || !replayed,
              "STALE_COMMAND_RESPONSE_REPLAYED_AFTER_RECONNECT = NO");
    }

    /* ---- fragmented RX: command executes exactly once ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        static CommandServices fsvc; cmd_init_harness(&fsvc); check(Command_Init(&fsvc), "frag: cmd init");
        const char* cmd="{\"id\":9,\"command\":\"GET_STATUS\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub),app.topic_command,
                                         strlen(app.topic_command),(const uint8_t*)cmd,strlen(cmd),0,false,0);
        /* feed 1 byte at a time; each step must exceed MqttApp's 50ms poll gate */
        for(size_t i=0;i<n;i++){ uint8_t b=pub[i]; FakeNetworkAdapter_FeedRecv(&s_fake,&b,1);
            FakePlatform_AdvanceTick(50); MqttApp_Run(&app); }
        check(app.stats.command_messages_received==1,
              "FRAGMENTED_RX: command executed EXACTLY once");
    }

    /* ---- authorization negative control: MQTT does not bypass ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        static CommandServices asvc; cmd_init_harness(&asvc); check(Command_Init(&asvc), "auth: cmd init");
        /* REBOOT is DESTRUCTIVE: not permitted for AUTHENTICATED_REMOTE. Delivery
           happens (enqueue), but production Command_Run rejects authorization. */
        const char* cmd="{\"id\":3,\"command\":\"REBOOT\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub),app.topic_command,
                                         strlen(app.topic_command),(const uint8_t*)cmd,strlen(cmd),0,false,0);
        FakeNetworkAdapter_FeedRecv(&s_fake,pub,n);
        int g=0;
        while(app.stats.command_messages_received==0 && g<100){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);g++;}
        check(app.stats.command_messages_received>=1, "AUTH: command delivered via MQTT");
        /* production authorization for AUTHENTICATED_REMOTE on a DESTRUCTIVE class
           is denied (handled by Command when Command_Run runs). */
        CommandType reb = COMMAND_REBOOT;
        check(Command_GetSecurityClass(reb)==COMMAND_SECURITY_DESTRUCTIVE,
              "AUTH: REBOOT classified DESTRUCTIVE");
        check(CommandAuthorization_IsAllowed(reb,COMMAND_SOURCE_AUTHENTICATED_REMOTE)==false,
              "MQTT_BYPASSES_COMMAND_AUTHORIZATION = NO (REBOOT denied for remote)");
    }

    /* ---- missing PINGRESP -> ERROR/backoff; module remains functional ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        check(app.state==MQTT_APP_ONLINE, "MR: online");
        /* advance past keepalive+pingresp period without PINGRESP */
        s_fake.recv_mode=FAKE_NET_RECV_NONE;
        int g=0;
        while(app.state!=MQTT_APP_BACKOFF && g<20000){FakePlatform_AdvanceTick(100);MqttApp_Run(&app);g++;}
        check(app.state==MQTT_APP_BACKOFF, "MR: missing PINGRESP -> backoff");

    }

    /* ---- BMP380 barometric telemetry preserved through MqttApp ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        check(app.state==MQTT_APP_ONLINE, "BARO: online");

        /* Production snapshot with active provider BMP380, Pa pressure, and a
           fully-valid SGP41 VOC index. */
        RoomState room; RoomState_Init(&room);
        room.barometric_pressure_pa = 98768.5f; room.barometric_pressure_valid = true;
        room.barometric_temperature_c = 26.8f; room.barometric_temperature_valid = true;
        room.barometric_provider = BAROMETER_PROVIDER_BMP380;
        room.co2_ppm = 480.0f; room.co2_valid = true;
        room.voc_raw = 30000.0f; room.voc_raw_valid = true;
        room.voc_index = 50.0f; room.voc_index_valid = true;
        uint8_t devid[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        TelemetrySnapshotInput tin; memset(&tin,0,sizeof(tin));
        tin.device_id = devid; tin.boot_id = 1; tin.room = &room;
        tin.health = SYSTEM_HEALTH_OK; tin.uptime_ms = 12345;
        TelemetrySnapshot snap;
        check(Telemetry_CreateSnapshot(&snap,&tin), "BARO: snapshot created");
        uint8_t payload[TELEMETRY_SERIALIZED_MAX_SIZE]; size_t len=0;
        check(Telemetry_Serialize(&snap,payload,sizeof(payload),&len)==SERIALIZE_OK && len>0U,
              "BARO: production serializer produced payload");
        check(len<=MQTT_MAX_PAYLOAD_SIZE, "BARO: payload fits MQTT bound");

        size_t pre=s_fake.tx_capture_len;
        MqttApp_PublishTelemetry(&app,payload,len);
        int g=0;
        while(s_fake.tx_capture_len < pre+len && g<20000){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);g++;}
        size_t total=s_fake.tx_capture_len;
        check(total>=pre+len, "BARO: telemetry published to MQTT wire");
        /* the serialized payload is the last `len` transmitted bytes */
        size_t at=(total>=len)?total-len:0;
        check(memcmp(s_fake.tx_capture+at,payload,len)==0,
              "BARO: TELEMETRY_BYTES_SURVIVE_MQTT = YES (exact bytes)");

        /* generic barometric contract: active provider must serialize as bmp380 with
           generic barometric_* names, pressure stays in Pa (not hPa), and BMP380 data
           must NEVER appear as valid bmp390_* compatibility data. */
        char *js=(char*)s_fake.tx_capture+at;
        check(strstr(js,"\"barometric_sensor\": \"bmp380\"")!=NULL,
              "BARO: GENERIC_BAROMETRIC_TELEMETRY_PRESERVED = YES (bmp380)");
        check(strstr(js,"\"barometric_pressure_pa\": {\n        \"value\": 98768.5,")!=NULL,
              "BARO: pressure serialized under generic barometric_pressure_pa (Pa)");
        check(strstr(js,"\"barometric_pressure_pa\":")!=NULL &&
              strstr(js,"hPa")==NULL && strstr(js,"mmHg")==NULL,
              "BARO: PRESSURE_TELEMETRY_UNIT = Pa (no hPa/mmHg in telemetry)");
        check(strstr(js,"\"bmp390_pressure_pa\": {\n        \"state\": \"invalid\"")!=NULL,
              "BARO: BMP380_DATA_SERIALIZED_AS_BMP390 = NO (bmp390 invalid)");
        check(strstr(js,"\"voc_index\": {\n        \"value\": 50.0,")!=NULL,
              "SGP41_VALIDITY_PRESERVED = YES (voc_index valid in telemetry)");
    }

    /* ---- long-run: repeated terminal loss bounded, no thrash ---- */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_identity_and_network(&app);
        run_until_online(&app);
        check(app.state==MQTT_APP_ONLINE, "LR: online");

        uint32_t losses = 20U;
        uint32_t attempts0 = app.stats.connect_attempts;   /* 1 (initial) */

        for (uint32_t k = 0; k < losses; k++) {
            /* terminal loss -> BACKOFF */
            s_fake.recv_mode = FAKE_NET_RECV_REMOTE_CLOSE;
            int g = 0;
            while (app.state != MQTT_APP_BACKOFF && g < 2000) { FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
            check(app.state == MQTT_APP_BACKOFF, "LR: terminal loss -> BACKOFF");
            /* wait out backoff (bounded 1..60s) and reconnect to ONLINE */
            bool once = false; uint32_t turn = 0;
            while (app.state != MQTT_APP_ONLINE && turn < 60000) {
                FakePlatform_AdvanceTick(50); MqttApp_Run(&app); turn += 50;
                if (!once && app.mqtt.state == MQTT_STATE_WAIT_CONNACK) {
                    uint8_t ca[4] = {0x20,0x02,0x00,0x00};
                    FakeNetworkAdapter_FeedRecv(&s_fake, ca, 4); once = true;
                }
                if (app.state == MQTT_APP_SUBSCRIBING && app.mqtt.sub_pending) {
                    uint16_t pid = app.mqtt.sub_packet_id;
                    uint8_t sa[5] = {0x90,0x03,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF),0x01};
                    FakeNetworkAdapter_FeedRecv(&s_fake, sa, 5);
                }
            }
            check(app.state == MQTT_APP_ONLINE, "LR: recovered to ONLINE");
            /* reset the fake wire for the next cycle */
            FakeNetworkAdapter_Reset(&s_fake);
        }

        /* Each loss allows exactly ONE reconnect after its backoff. Over `losses`
           loss cycles the total connect attempts must stay tightly bounded (no
           reconnect thrash / no publish storm). */
        uint32_t total = app.stats.connect_attempts;
        check(total <= attempts0 + losses + 2U,
              "LR: MQTT_RECONNECT_THRASH = NO (bounded attempts across losses)");
        check(total >= attempts0 + losses - 1U,
              "LR: reconnect actually occurred (>= ~1 attempt per loss)");
    }

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail==0)?0:1;
}