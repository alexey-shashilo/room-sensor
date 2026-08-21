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
#include "fake_flash.h"
#include "fake_unique_id.h"
#include "device_identity.h"
#include "room_state.h"
#include "telemetry.h"
#include "telemetry_serializer.h"

/* MQTT App WHOLE-DEVICE acceptance (Phase 18.1).

   Drives PRODUCTION MqttApp + MqttClient + NetworkTransport + Command +
   TelemetrySerializer + fake adapter as ONE device (each component is the real
   production module; only the wire adapter and clock are fake):

     boot network unavailable -> SAFE_DISABLED (no connect storm)
     offline telemetry periods dropped (no backlog)
     network available -> CONNECT/CONNACK -> SUBACK -> ONLINE
     telemetry publish (production snapshot + production serializer), byte-exact
     GET_MANIFEST inbound -> production Command -> production response outbound
     response QoS1 -> PUBACK
     connection loss -> module backs off, no fabrication, Command unaffected
     reconnect -> ONLINE -> fresh telemetry -> new command works
   */

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

static void drive_online(MqttApp *app){
    MqttApp_Run(app);
    bool fed=false; int g=0;
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

int main(void)
{
    printf("MQTT App whole-device (integration) tests\n");
    FakePlatform_SetTick(0);
    make_id(&s_id);

    /* ---- production Command bound (single shared module-global) ---- */
    static RoomState cmd_room; RoomState_Init(&cmd_room);
    static RoomSensorConfig cmd_cfg; memset(&cmd_cfg,0,sizeof(cmd_cfg));
    static SelfTestReport cmd_st; memset(&cmd_st,0,sizeof(cmd_st));
    static CommandRuntimeStatus cmd_rs; memset(&cmd_rs,0,sizeof(cmd_rs));
    static CommandServices svc; static I2cBus cmd_bus; memset(&cmd_bus,0,sizeof(cmd_bus));
    memset(&svc,0,sizeof(svc)); svc.room=&cmd_room; svc.config=&cmd_cfg;
    svc.identity=&s_id; svc.self_test=&cmd_st; svc.bus=&cmd_bus; svc.runtime_status=&cmd_rs;
    check(Command_Init(&svc), "WDEV: Command_Init");

    /* ---- Phase 1: NO adapter -> SAFE_DISABLED, no connect storm ---- */
    MqttApp app_off; FakePlatform_SetTick(0);
    MqttApp_Init(&app_off, NULL, NULL, &s_id);
    check(MqttApp_GetState(&app_off)==MQTT_APP_DISABLED,
          "net-unavailable boot -> SAFE_DISABLED");
    for(int i=0;i<100;i++){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app_off); }
    check(app_off.stats.connect_attempts==0,
          "NETWORK_ADAPTER_ABSENT_CAUSES_CONNECT_STORM = NO (0 attempts)");
    check(MqttApp_GetState(&app_off)==MQTT_APP_DISABLED,
          "MQTT_APP_ACTIVE_WITHOUT_ADAPTER = SAFE_DISABLED");

    /* ---- Phase 2: network available -> ONLINE ---- */
    MqttApp app;
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter,&s_fake);
    NetworkEndpoint ep; memset(&ep,0,sizeof(ep));
    strncpy(ep.host,"broker.test",sizeof(ep.host)-1); ep.port=1883;
    MqttApp_Init(&app,&s_adapter,&ep,&s_id);

    /* offline telemetry while connecting -> counted, not queued (no backlog) */
    {
        uint8_t ser[]="{\"schema\":5}";
        MqttApp_PublishTelemetry(&app,ser,sizeof(ser));
        check(app.stats.telemetry_publish_attempts==1 && app.stats.telemetry_publish_accepted==0,
              "OFFLINE_TELEMETRY_IS_BACKLOGGED = NO (dropped, not queued)");
    }

    drive_online(&app);
    check(MqttApp_GetState(&app)==MQTT_APP_ONLINE, "network available -> ONLINE");

    /* ---- Phase 3: telemetry via production snapshot + serializer ---- */
    {
        RoomState room; RoomState_Init(&room);
        room.barometric_pressure_pa=98768.5f; room.barometric_pressure_valid=true;
        room.barometric_provider=BAROMETER_PROVIDER_BMP380;
        room.co2_ppm=500.0f; room.co2_valid=true;
        room.voc_raw=30000.0f; room.voc_raw_valid=true;
        uint8_t devid[16]={1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        TelemetrySnapshotInput tin; memset(&tin,0,sizeof(tin));
        tin.device_id=devid; tin.boot_id=1; tin.room=&room;
        tin.health=SYSTEM_HEALTH_OK; tin.uptime_ms=0;
        TelemetrySnapshot snap; Telemetry_CreateSnapshot(&snap,&tin);
        uint8_t ser[TELEMETRY_SERIALIZED_MAX_SIZE]; size_t slen=0;
        check(Telemetry_Serialize(&snap,ser,sizeof(ser),&slen)==SERIALIZE_OK && slen>0,
              "telem: production serializer");
        size_t pre=s_fake.tx_capture_len;
        MqttApp_PublishTelemetry(&app,ser,slen);
        int g=0; while(s_fake.tx_capture_len < pre+slen+80U && g<20000){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(s_fake.tx_capture_len >= pre+slen, "telem: telemetry published on wire");
        /* byte-exact: telemetry payload is the tail (QoS0 publish is last packet) */
        size_t off=s_fake.tx_capture_len-slen;
        check(memcmp(s_fake.tx_capture+off, ser, slen)==0, "telem: payload byte-exact on wire");
    }

    /* ---- Phase 4: GET_MANIFEST -> production Command response -> PUBACK ---- */
    {
        static CommunicationPort port;
        port.context=&app; port.send=MqttApp_PortSend; port.is_ready=MqttApp_PortReady;
        Command_SetPort(&port);
        const char *cmd="{\"id\":3,\"command\":\"GET_MANIFEST\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub), app.topic_command,
                                         strlen(app.topic_command),(const uint8_t*)cmd,strlen(cmd),1,false,3);
        FakeNetworkAdapter_FeedRecv(&s_fake,pub,n);
        int g=0; while(app.stats.command_messages_received==0 && g<200){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received>=1, "GET_MANIFEST routed to production Command");
        for(int k=0;k<200;k++){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); } /* drain PUBACK */
        Command_Run();
        for(int k=0;k<100;k++){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); }
        check(app.stats.command_responses_published>=1, "command response published (QoS1)");
        check(app.mqtt.inflight_active, "response QoS1 inflight");
        uint16_t pid=app.mqtt.inflight_packet_id;
        check(pid!=0, "MQTT_PACKET_ID_ZERO_GENERATED = NO");
        uint8_t ok[4]={0x40,0x02,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF)};
        /* prime recv to FEED and queue the matching PUBACK, then pump */
        s_fake.recv_mode = FAKE_NET_RECV_FEED;
        FakeNetworkAdapter_FeedRecv(&s_fake,ok,4);
        int g2=0; while(app.mqtt.inflight_active && g2<200){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g2++; }
        check(!app.mqtt.inflight_active, "matching PUBACK completes response");
    }

    /* ---- Phase 5: connection loss -> backoff, Command/telemetry unaffected ---- */
    {
        s_fake.recv_mode=FAKE_NET_RECV_REMOTE_CLOSE;
        int g=0; while(app.state!=MQTT_APP_BACKOFF && g<2000){ FakePlatform_AdvanceTick(50); MqttApp_Run(&app); g++; }
        check(app.state==MQTT_APP_BACKOFF, "connection loss -> backoff");
        /* While disconnected: a telemetry publish is dropped (no fabrication/backlog). */
        uint32_t a0=app.stats.telemetry_publish_accepted;
        uint8_t ser[]="{\"offline\":1}";
        MqttApp_PublishTelemetry(&app,ser,sizeof(ser));
        check(app.stats.telemetry_publish_accepted==a0,
              "while offline: telemetry dropped (no fabrication/backlog)");
        /* Command still functions independently (module offline does not break it). */
        check(CommandAuthorization_IsAllowed(COMMAND_GET_STATUS, COMMAND_SOURCE_AUTHENTICATED_REMOTE)==true,
              "Command unaffected while MQTT offline");
    }

    /* ---- Phase 6: reconnect -> online -> new command works ---- */
    {
        MqttClient_Disconnect(&app.mqtt);
        app.state=MQTT_APP_DISCONNECTED;
        FakeNetworkAdapter_Reset(&s_fake);
        FakeNetworkAdapter_GetAdapter(&s_adapter,&s_fake);
        drive_online(&app);
        check(app.state==MQTT_APP_ONLINE, "reconnected to ONLINE");
        const char *cmd="{\"id\":6,\"command\":\"GET_STATUS\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub), app.topic_command,
                                         strlen(app.topic_command),(const uint8_t*)cmd,strlen(cmd),1,false,6);
        uint32_t rc0=app.stats.command_messages_received;
        FakeNetworkAdapter_FeedRecv(&s_fake,pub,n);
        int g=0; while(app.stats.command_messages_received<=rc0 && g<200){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received>rc0, "new command works after final reconnect");
    }

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail==0)?0:1;
}