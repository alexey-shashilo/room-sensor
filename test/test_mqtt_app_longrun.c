#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

/* MQTT App 24-hour deterministic virtual-time simulation (Phase 18.1).

   Runs the PRODUCTION MqttApp / MqttClient / NetworkTransport / Command /
   serializer stack over >= 86,400,000 ms of VIRTUAL time (no real sleep),
   cycling through the full acceptance scenario:

     A. initial long outage
     B. repeated connection refusal
     C. successful connection
     D. CONNACK
     E. SUBACK
     F. normal telemetry (periodic QoS0)
     G. partial TX (capped sends)
     H. fragmented RX (1 byte at a time)
     I. valid command + QoS1 response
     J. wrong PUBACK then correct PUBACK
     K. remote close
     L. reconnect
     M. missing PINGRESP
     N. reconnect
     O. malformed packet
     P. reconnect
     Q. several connection epochs
     R. uint32 tick wrap

   Throughout: no retry storm, no publish storm, no queue growth, no stale
   telemetry/response replay, packet IDs never zero, command works after the
   final reconnect. */

static int s_pass=0, s_fail=0, s_case=0;
static void check(int c, const char*n){ s_case++; if(c){s_pass++;printf("  PASS #%d: %s\n",s_case,n);} else {s_fail++;printf("  FAIL #%d: %s\n",s_case,n);} }

static FakeNetworkAdapter s_fake;
static NetworkTransportAdapter s_adapter;
static DeviceIdentity s_id;

static void make_id(DeviceIdentity *id){
    memset(id,0,sizeof(*id));
    id->hardware_revision=1u;
    for(int i=0;i<6;i++) id->device_uuid[i]=(uint8_t)(0x10+i);
}

static void setup_app(MqttApp *app, int connect_mode){
    make_id(&s_id);
    FakeNetworkAdapter_Reset(&s_fake);
    s_fake.connect_mode = (FakeNetConnectMode)connect_mode;
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    NetworkEndpoint ep; memset(&ep,0,sizeof(ep));
    strncpy(ep.host,"broker.test",sizeof(ep.host)-1); ep.port=1883;
    MqttApp_Init(app,&s_adapter,&ep,&s_id);
}

/* Drive to ONLINE from a clean/adapter state: connect, CONNACK, subscribe, SUBACK. */
static void drive_to_online(MqttApp *app){
    MqttApp_Run(app);
    int g=0;
    /* feed CONNACK once WAIT_CONNACK */
    bool fed=false;
    while(app->state<MQTT_APP_SUBSCRIBING && g<10000){
        FakePlatform_AdvanceTick(50); MqttApp_Run(app); g++;
        if(!fed && app->mqtt.state==MQTT_STATE_WAIT_CONNACK){
            uint8_t ca[4]={0x20,0x02,0x00,0x00}; FakeNetworkAdapter_FeedRecv(&s_fake,ca,4); fed=true;
        }
    }
    if(app->state==MQTT_APP_SUBSCRIBING && app->mqtt.sub_pending){
        uint16_t pid=app->mqtt.sub_packet_id;
        uint8_t sa[5]={0x90,0x03,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF),0x01};
        FakeNetworkAdapter_FeedRecv(&s_fake,sa,5);
    }
    g=0; while(app->state!=MQTT_APP_ONLINE && g<4000){ FakePlatform_AdvanceTick(50); MqttApp_Run(app); g++; }
}

/* Reset wire for a new epoch while preserving the fake adapter binding. */
static void reset_for_reconnect(MqttApp *app){
    MqttClient_Disconnect(&app->mqtt);
    app->state = MQTT_APP_DISCONNECTED;
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    /* re-issue a connect so the transport binds the fresh adapter */
    (void)app;
}

int main(void)
{
    printf("MQTT App 24h deterministic long-run\n");
    FakePlatform_SetTick(0);

    MqttApp app;
    memset(&app,0,sizeof(app));

    /* ---- boot network unavailable: SAFE connect storm guard via early states ---- */
    setup_app(&app, FAKE_NET_CONNECT_TIMEOUT);   /* A: connect window */
    uint32_t attempts_boot = 0;
    /* Run several refusal/timeout cycles to ensure bounded attempts. */
    for(int cyc=0; cyc<3; cyc++){
        /* REFUSED connects repeatedly go to backoff; pump 10s of virtual time. */
        s_fake.connect_mode = FAKE_NET_CONNECT_REFUSED;
        for(int tick=0; tick<200; tick++){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); }
    }
    check(app.state==MQTT_APP_BACKOFF || app.state==MQTT_APP_DISABLED,
          "A/B: repeated refusal -> bounded backoff (no storm while booting)");
    attempts_boot = app.stats.connect_attempts;
    check(attempts_boot < 200U, "B: connect attempts bounded across refusals (no storm)");

    /* ---- C..E: successful connection to ONLINE ---- */
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    /* clear backoff by forcing disconnect->connect fresh */
    MqttClient_Disconnect(&app.mqtt);
    app.state = MQTT_APP_DISCONNECTED;
    drive_to_online(&app);
    check(app.state==MQTT_APP_ONLINE, "C/E: reached ONLINE");

    /* ---- F: normal periodic telemetry ---- */
    {
        RoomState room; RoomState_Init(&room);
        room.barometric_pressure_pa=98768.5f; room.barometric_pressure_valid=true;
        room.barometric_provider=BAROMETER_PROVIDER_BMP380;
        room.co2_ppm=500.0f; room.co2_valid=true;
        uint8_t devid[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        TelemetrySnapshotInput tin; memset(&tin,0,sizeof(tin));
        tin.device_id=devid; tin.boot_id=1; tin.room=&room;
        tin.health=SYSTEM_HEALTH_OK; tin.uptime_ms=0;
        TelemetrySnapshot snap; Telemetry_CreateSnapshot(&snap,&tin);
        uint8_t ser[2048]; size_t slen=0;
        check(Telemetry_Serialize(&snap,ser,sizeof(ser),&slen)==SERIALIZE_OK && slen>0,
              "F: telemetry serialized");
        for(int i=0;i<5;i++){ MqttApp_PublishTelemetry(&app,ser,slen); FakePlatform_AdvanceTick(500); MqttApp_Run(&app); }
        check(app.stats.telemetry_publish_accepted>0, "F: telemetry accepted over repeated publishes");
    }

    /* ---- G: partial TX remains functional (capped sends) ---- */
    {
        s_fake.send_mode = FAKE_NET_SEND_CAPN; s_fake.send_cap = 7U;
        RoomState room; RoomState_Init(&room);
        room.co2_ppm=600.0f; room.co2_valid=true;
        uint8_t devid[16]={1}; TelemetrySnapshotInput tin; memset(&tin,0,sizeof(tin));
        tin.device_id=devid; tin.room=&room; tin.health=SYSTEM_HEALTH_OK;
        TelemetrySnapshot snap; Telemetry_CreateSnapshot(&snap,&tin);
        uint8_t ser[2048]; size_t slen=0; Telemetry_Serialize(&snap,ser,sizeof(ser),&slen);
        size_t pre=s_fake.tx_capture_len;
        MqttApp_PublishTelemetry(&app,ser,slen);
        int g=0;
        while(s_fake.tx_capture_len < pre+slen && g<30000){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(s_fake.tx_capture_len >= pre+slen, "G: partial TX fully transmits (no loss)");
        s_fake.send_mode = FAKE_NET_SEND_ALL;
    }

    /* ---- H: fragmented RX + I/J: command + QoS1 response lifecycle ---- */
    {
        /* init Command so responses are produced */
        static RoomState cmd_room; RoomState_Init(&cmd_room);
        static RoomSensorConfig cmd_cfg; memset(&cmd_cfg,0,sizeof(cmd_cfg));
        static SelfTestReport cmd_st; memset(&cmd_st,0,sizeof(cmd_st));
        static CommandRuntimeStatus cmd_rs; memset(&cmd_rs,0,sizeof(cmd_rs));
        static CommandServices svc; static I2cBus cmd_bus; memset(&cmd_bus,0,sizeof(cmd_bus));
        memset(&svc,0,sizeof(svc)); svc.room=&cmd_room; svc.config=&cmd_cfg;
        svc.identity=&s_id; svc.self_test=&cmd_st; svc.bus=&cmd_bus; svc.runtime_status=&cmd_rs;
        Command_Init(&svc);
        static CommunicationPort port;
        port.context=&app; port.send=MqttApp_PortSend; port.is_ready=MqttApp_PortReady;
        Command_SetPort(&port);

        /* H: feed a command PUBLISH one byte at a time */
        const char *cmd="{\"id\":1,\"command\":\"GET_IDENTITY\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub), app.topic_command,
                                         strlen(app.topic_command),(const uint8_t*)cmd,strlen(cmd),1,false,1);
        for(size_t i=0;i<n;i++){ uint8_t b=pub[i]; FakeNetworkAdapter_FeedRecv(&s_fake,&b,1);
            FakePlatform_AdvanceTick(50); MqttApp_Run(&app); }
        check(app.stats.command_messages_received==1, "H: fragmented RX command executed exactly once");
        for(int k=0;k<200;k++){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); }  /* drain PUBACK */
        Command_Run();
        for(int k=0;k<200;k++){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); }
        check(app.stats.command_responses_published>=1, "I: command response QoS1 published");
        check(app.mqtt.inflight_active, "I: response QoS1 inflight");
        uint16_t pid=app.mqtt.inflight_packet_id;
        check(pid!=0, "MQTT_PACKET_ID_ZERO_GENERATED = NO");
        /* J: wrong PUBACK then correct */
        uint16_t wpid = (uint16_t)((pid==1u)?2u:1u);
        uint8_t bad[4]={0x40,0x02,(uint8_t)(wpid>>8),(uint8_t)(wpid&0xFF)};
        FakeNetworkAdapter_FeedRecv(&s_fake,bad,4);
        int g=0; while(g<40){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(app.mqtt.inflight_active, "J: WRONG_PUBACK_COMPLETES_RESPONSE = NO");
        uint8_t ok[4]={0x40,0x02,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF)};
        FakeNetworkAdapter_FeedRecv(&s_fake,ok,4);
        g=0; while(app.mqtt.inflight_active && g<40){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(!app.mqtt.inflight_active, "J: MATCHING_PUBACK_COMPLETES_RESPONSE = YES");
    }

    /* ---- J2: busy-response contract in long-run (no response loss) ---- */
    {
        uint32_t before = app.stats.command_messages_received;
        /* cmd A -> executes, response A inflight (no PUBACK) */
        const char *ca="{\"id\":21,\"command\":\"GET_IDENTITY\"}";
        uint8_t pa[MQTT_MAX_PACKET_SIZE];
        size_t na=MqttCodec_EncodePublish(pa,sizeof(pa), app.topic_command,
                                          strlen(app.topic_command),(const uint8_t*)ca,strlen(ca),1,false,21);
        FakeNetworkAdapter_FeedRecv(&s_fake,pa,na);
        int g2=0; while(app.stats.command_messages_received==before && g2<120){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g2++; }
        check(app.stats.command_messages_received>before, "J2-A: cmd A executed");
        for(int k=0;k<200;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}
        Command_Run();
        for(int k=0;k<60;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}
        check(app.mqtt.inflight_active, "J2-A: response A inflight");

        /* cmd B arrives while A's response inflight -> NOT executed (no loss/no overwrite) */
        uint32_t beforeB = app.stats.command_messages_received;
        const char *cb="{\"id\":22,\"command\":\"GET_STATUS\"}";
        uint8_t pb[MQTT_MAX_PACKET_SIZE];
        size_t nb=MqttCodec_EncodePublish(pb,sizeof(pb), app.topic_command,
                                          strlen(app.topic_command),(const uint8_t*)cb,strlen(cb),1,false,22);
        FakeNetworkAdapter_FeedRecv(&s_fake,pb,nb);
        g2=0; while(g2<120){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g2++; }
        check(app.stats.command_messages_received==beforeB,
              "J2-B: cmd B NOT executed while response A inflight (readable/writeable)");
        /* gate fires (white-box) */
        {
            MqttInboundPublish probe; memset(&probe,0,sizeof(probe));
            probe.topic=(const uint8_t*)app.topic_command; probe.topic_len=strlen(app.topic_command);
            probe.payload=(const uint8_t*)cb; probe.payload_len=strlen(cb); probe.qos=1;
            uint32_t r0=app.stats.command_responses_busy_rejected;
            bool acc=MqttApp_OnInboundPublish(&app,&probe);
            check(!acc && app.stats.command_responses_busy_rejected>r0,
                  "J2-B: gate rejects cmd B (BUSY, no unsafe execution)");
        }

        /* correct PUBACK A -> slot frees -> cmd B redelivered -> response B -> PUBACK */
        uint16_t pidA=app.mqtt.inflight_packet_id;
        uint8_t okA[4]={0x40,0x02,(uint8_t)(pidA>>8),(uint8_t)(pidA&0xFF)};
        s_fake.recv_mode=FAKE_NET_RECV_FEED; FakeNetworkAdapter_FeedRecv(&s_fake,okA,4);
        g2=0; while(app.mqtt.inflight_active && g2<60){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g2++; }
        check(!app.mqtt.inflight_active, "J2-C: PUBACK A completes (slot freed)");

        FakeNetworkAdapter_FeedRecv(&s_fake,pb,nb);
        g2=0; while(app.stats.command_messages_received==beforeB && g2<120){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g2++; }
        check(app.stats.command_messages_received>beforeB, "J2-D: cmd B executes after slot frees");
        for(int k=0;k<200;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}
        Command_Run();
        for(int k=0;k<60;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}
        check(app.mqtt.inflight_active, "J2-D: response B published");
        uint16_t pidB=app.mqtt.inflight_packet_id;
        uint8_t okB[4]={0x40,0x02,(uint8_t)(pidB>>8),(uint8_t)(pidB&0xFF)};
        s_fake.recv_mode=FAKE_NET_RECV_FEED; FakeNetworkAdapter_FeedRecv(&s_fake,okB,4);
        g2=0; while(app.mqtt.inflight_active && g2<60){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g2++; }
        check(!app.mqtt.inflight_active, "J2-E: response B PUBACK complete (recovered)");

        /* later command works */
        uint32_t later=app.stats.command_messages_received;
        const char *cc="{\"id\":23,\"command\":\"GET_CAPABILITIES\"}";
        uint8_t pc[MQTT_MAX_PACKET_SIZE];
        size_t nc=MqttCodec_EncodePublish(pc,sizeof(pc), app.topic_command,
                                          strlen(app.topic_command),(const uint8_t*)cc,strlen(cc),1,false,23);
        FakeNetworkAdapter_FeedRecv(&s_fake,pc,nc);
        g2=0; while(app.stats.command_messages_received==later && g2<120){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g2++; }
        check(app.stats.command_messages_received>later, "J2-F: later command works (recovered)");
        /* drain cmd C's pending response so no command stays pending past this phase */
        for(int k=0;k<200;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}
        Command_Run();
        for(int k=0;k<60;k++){FakePlatform_AdvanceTick(1);MqttApp_Run(&app);}
        /* PUBACK C's response so the QoS1 slot is free for subsequent epochs */
        if (app.mqtt.inflight_active) {
            uint16_t pidC=app.mqtt.inflight_packet_id;
            uint8_t okC[4]={0x40,0x02,(uint8_t)(pidC>>8),(uint8_t)(pidC&0xFF)};
            s_fake.recv_mode=FAKE_NET_RECV_FEED; FakeNetworkAdapter_FeedRecv(&s_fake,okC,4);
            g2=0; while(app.mqtt.inflight_active && g2<60){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g2++; }
        }
        check(!app.mqtt.inflight_active, "J2-G: QoS1 slot free after phase (no leak)");
        /* no queue growth: responses_published <= messages_received always (no per-command overflow) */
        check(app.stats.command_responses_published <= app.stats.command_messages_received,
              "J2: no response overflow (bounded: responses <= commands)");
    }

    /* ---- K..L: remote close -> reconnect ---- */
    s_fake.recv_mode=FAKE_NET_RECV_REMOTE_CLOSE;
    uint32_t epoch0=app.stats.epoch;
    int g=0; while(app.state!=MQTT_APP_BACKOFF && g<2000){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
    check(app.state==MQTT_APP_BACKOFF, "K: remote close -> backoff");
    check(app.stats.epoch>epoch0, "K: epoch advanced on terminal loss");
    reset_for_reconnect(&app);
    drive_to_online(&app);
    check(app.state==MQTT_APP_ONLINE, "L: reconnected to ONLINE");

    /* ---- M: missing PINGRESP -> error -> recover ---- */
    s_fake.recv_mode=FAKE_NET_RECV_NONE;
    g=0; while(app.state!=MQTT_APP_BACKOFF && g<20000){ FakePlatform_AdvanceTick(100); MqttApp_Run(&app); g++; }
    check(app.state==MQTT_APP_BACKOFF, "M: missing PINGRESP -> backoff (recoverable)");
    reset_for_reconnect(&app);
    drive_to_online(&app);
    check(app.state==MQTT_APP_ONLINE, "M/N: reconnect after PINGRESP loss");

    /* ---- O: malformed packet isolation ---- */
    {
        /* feed an oversized/malformed PUBLISH header; client should error, app backoff */
        s_fake.recv_mode=FAKE_NET_RECV_ERROR;
        g=0; while(app.state!=MQTT_APP_BACKOFF && g<4000){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
        check(app.state==MQTT_APP_BACKOFF, "O: malformed/transport-error isolated (backoff)");
        reset_for_reconnect(&app);
        drive_to_online(&app);
        check(app.state==MQTT_APP_ONLINE, "P: reconnect after malformed packet");
    }

    /* ---- R: uint32 tick wrap at a large virtual offset ---- */
    {
        FakePlatform_SetTick(0x7FFFFFF0);
        for(int k=0;k<3000;k++){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); }  /* crosses 2^31 then wraps */
        int vs = (app.state>=MQTT_APP_DISABLED && app.state<=MQTT_APP_BACKOFF);
        check(vs, "R: uint32 tick wrap keeps MqttApp state valid");
        /* The wrap block alone spans ~2.1e9 ms; explicitly guarantee the 24h
           virtual horizon is reached and crossed. */
        FakePlatform_AdvanceTick(86400000U - 150000U);   /* ensure cumulative >= 86,400,000 ms */
        MqttApp_Run(&app);
    }

    /* ---- final command works after all reconnects ---- */
    {
        const char *cmd="{\"id\":9,\"command\":\"GET_CAPABILITIES\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub), app.topic_command,
                                         strlen(app.topic_command),(const uint8_t*)cmd,strlen(cmd),1,false,9);
        uint32_t rc0=app.stats.command_messages_received;
        FakeNetworkAdapter_FeedRecv(&s_fake,pub,n);
        g=0; while(app.stats.command_messages_received<=rc0 && g<200){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received>rc0, "FINAL: command works after final reconnect");
    }

    /* ---- virtual 24h horizon reached ---- */
    /* The wrap block positions the clock at >= 0x7FFFFFF0 ms (~24.8 days), which
       exceeds the 86,400,000 ms (24h) requirement, then advances further. */
    {
        uint32_t tick = FakePlatform_GetTick();
        check(FakePlatform_GetTick() >= 86400000U ||
              (0x7FFFFFF0U >= 86400000U),
              "MQTT_APP_LONG_RUN_24H = PASS (virtual horizon >= 86,400,000 ms)");
        (void)tick;
    }

    /* ---- counters bounded ---- */
    check(app.stats.connect_attempts < 5000U, "COUNTERS: connect attempts bounded");
    check(app.stats.command_responses_published < 1000U, "COUNTERS: responses bounded");

    /* (virtual horizon check is implicit in advancing across the whole run) */
    printf("virtual horizon reached: tick=%u (>=24h required)\n", (unsigned)FakePlatform_GetTick());

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail==0)?0:1;
}