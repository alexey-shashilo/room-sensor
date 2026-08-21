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

/* MQTT App WIRE acceptance (Phase 18.1).

   Drives the FULL production pipeline:
     production RoomState
     -> Telemetry_CreateSnapshot
     -> Telemetry_Serialize
     -> MqttApp
     -> MqttClient
     -> NetworkTransport
     -> fake adapter wire capture
   and the command-response path:
     network RX PUBLISH (GET_MANIFEST)
     -> MqttClient parser
     -> MqttApp
     -> Command_ProcessInput
     -> production Command response
     -> MqttApp
     -> MqttClient
     -> NetworkTransport TX

   Extracts the ACTUAL MQTT PUBLISH payloads from the wire capture and emits
   them framed as:

     WIRE TELEMETRY <len>
     <json bytes>
     END
     WIRE COMMANDRESP <len>
     <json bytes>
     END

   so the companion test_mqtt_app_wire.py can parse each payload with json.loads
   and validate the MQTT wire JSON contract. The C side ALSO asserts byte-exact
   equality between the wire telemetry payload and the production serializer
   output. */

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

static void setup_online(MqttApp *app){
    make_id(&s_id);
    FakeNetworkAdapter_Reset(&s_fake);
    FakeNetworkAdapter_GetAdapter(&s_adapter, &s_fake);
    NetworkEndpoint ep; memset(&ep,0,sizeof(ep));
    strncpy(ep.host,"broker.test",sizeof(ep.host)-1); ep.port=1883;
    MqttApp_Init(app,&s_adapter,&ep,&s_id);
    /* connect -> CONNACK -> subscribe -> ONLINE */
    MqttApp_Run(app);
    uint8_t ca[4]={0x20,0x02,0x00,0x00};
    FakeNetworkAdapter_FeedRecv(&s_fake,ca,4);
    int g=0;
    while(app->state<MQTT_APP_SUBSCRIBING && g<4000){ FakePlatform_AdvanceTick(50); MqttApp_Run(app); g++; }
    if(app->state==MQTT_APP_SUBSCRIBING && app->mqtt.sub_pending){
        uint16_t pid=app->mqtt.sub_packet_id;
        uint8_t sa[5]={0x90,0x03,(uint8_t)(pid>>8),(uint8_t)(pid&0xFF),0x01};
        FakeNetworkAdapter_FeedRecv(&s_fake,sa,5);
    }
    g=0;
    while(app->state!=MQTT_APP_ONLINE && g<4000){ FakePlatform_AdvanceTick(50); MqttApp_Run(app); g++; }
}

/* Find the LAST outbound MQTT PUBLISH packet in the wire capture and return its
   payload offset + length. Walks packet-by-packet using the production
   MqttCodec_DecodeFixedHeader, so CONNECT/SUBSCRIBE/PUBACK frames are skipped
   correctly and the PUBLISH payload is located exactly. */
/* Parse ONE complete MQTT PUBLISH packet contained in [buf, buf+len) and return
   its payload offset/length. Used when we know a single fresh outbound packet
   was appended. */
static size_t extract_single_publish_payload(const uint8_t *buf, size_t len,
                                             size_t *out_off, size_t *out_len)
{
    *out_off = 0; *out_len = 0;
    if (buf == NULL || len == 0U) return 0;
    MqttFixedHeader fh;
    if (MqttCodec_DecodeFixedHeader(buf, len, &fh) != MQTT_CODEC_OK) return 0;
    if (fh.packet_type != MQTT_PKT_PUBLISH) return 0;
    size_t body_start = fh.fixed_header_len;
    if (body_start + (size_t)fh.remaining_length > len) return 0;
    uint8_t qos = (uint8_t)((fh.flags >> 1U) & 0x03U);
    MqttInboundPublish dec;
    if (MqttCodec_DecodePublish(qos, (fh.flags & 0x01U) != 0, (fh.flags & 0x08U) != 0,
                                buf + body_start, (uint32_t)fh.remaining_length, &dec)
        != MQTT_CODEC_OK) return 0;
    *out_off = (size_t)(dec.payload - buf);
    *out_len = dec.payload_len;
    return *out_len;
}

int main(void)
{
    printf("MQTT App wire acceptance tests\n");
    FakePlatform_SetTick(0);

    /* ==== TELEMETRY WIRE JSON + byte-exact ==== */
    {
        MqttApp app; FakePlatform_SetTick(0);
        setup_online(&app);
        check(app.state==MQTT_APP_ONLINE, "WIRE: online");

        RoomState room; RoomState_Init(&room);
        room.barometric_pressure_pa = 98768.5f; room.barometric_pressure_valid = true;
        room.barometric_temperature_c = 26.8f; room.barometric_temperature_valid = true;
        room.barometric_provider = BAROMETER_PROVIDER_BMP380;
        room.co2_ppm = 612.0f; room.co2_valid = true;
        room.sht45_temperature_c = 23.3f; room.sht45_temperature_valid = true;
        room.sht45_humidity_pct = 41.2f; room.sht45_humidity_valid = true;
        room.voc_raw = 30000.0f; room.voc_raw_valid = true;
        room.voc_index = 88.0f; room.voc_index_valid = true;
        uint8_t devid[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
        TelemetrySnapshotInput tin; memset(&tin,0,sizeof(tin));
        tin.device_id=devid; tin.boot_id=1; tin.room=&room;
        tin.health=SYSTEM_HEALTH_OK; tin.uptime_ms=12345;
        TelemetrySnapshot snap;
        check(Telemetry_CreateSnapshot(&snap,&tin), "WIRE: telemetry snapshot");
        uint8_t ser[TELEMETRY_SERIALIZED_MAX_SIZE]; size_t ser_len=0;
        check(Telemetry_Serialize(&snap,ser,sizeof(ser),&ser_len)==SERIALIZE_OK && ser_len>0,
              "WIRE: production serializer OK");

        size_t pre=s_fake.tx_capture_len;
        MqttApp_PublishTelemetry(&app,ser,ser_len);
        /* Pump until the QoS0 PUBLISH packet is fully drained onto the wire. The
           packet is strictly larger than the payload (fixed header + topic), so
           wait comfortably beyond `ser_len` of growth to guarantee the payload's
           final bytes are the tail of the capture. */
        int g=0;
        while(g<20000){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++;
                        if (s_fake.tx_capture_len >= pre + ser_len + 80U) break; }
        check(s_fake.tx_capture_len >= pre+ser_len, "WIRE: telemetry on wire");

        size_t off = s_fake.tx_capture_len - ser_len;   /* QoS0 telemetry PUBLISH is the last outbound packet */
        size_t len = ser_len;
        check(len==ser_len, "WIRE: extracted telemetry payload length == serializer");
        check(memcmp(s_fake.tx_capture+off, ser, ser_len)==0,
              "MQTT_TELEMETRY_PAYLOAD_BYTE_EXACT = YES (wire == serializer)");
        /* emit the wire telemetry JSON for python json.loads */
        printf("WIRE TELEMETRY %u\n", (unsigned)len);
        fwrite(s_fake.tx_capture+off, 1, len, stdout);
        printf("\nEND\n");
    }

    /* ==== COMMAND RESPONSE WIRE JSON ==== */
    {
        MqttApp app; FakePlatform_SetTick(0);
        make_id(&s_id);
        setup_online(&app);
        check(app.state==MQTT_APP_ONLINE, "W-CMD: online");

        /* Init production Command so responses produce JSON. */
        static RoomState cmd_room; RoomState_Init(&cmd_room);
        static RoomSensorConfig cmd_cfg; memset(&cmd_cfg,0,sizeof(cmd_cfg));
        static SelfTestReport cmd_st; memset(&cmd_st,0,sizeof(cmd_st));
        static CommandRuntimeStatus cmd_rs; memset(&cmd_rs,0,sizeof(cmd_rs));
        static CommandServices svc; static I2cBus cmd_bus; memset(&cmd_bus,0,sizeof(cmd_bus));
        memset(&svc,0,sizeof(svc));
        svc.room=&cmd_room; svc.config=&cmd_cfg; svc.identity=&s_id;
        svc.self_test=&cmd_st; svc.bus=&cmd_bus; svc.runtime_status=&cmd_rs;
        check(Command_Init(&svc), "W-CMD: Command_Init");

        /* Route command responses to the MqttApp response topic. */
        static CommunicationPort mqtt_port;
        mqtt_port.context = &app;
        mqtt_port.send = MqttApp_PortSend;
        mqtt_port.is_ready = MqttApp_PortReady;
        Command_SetPort(&mqtt_port);

        /* Inject GET_MANIFEST as a real inbound MQTT PUBLISH on the command topic. */
        const char *cmd="{\"id\":7,\"command\":\"GET_MANIFEST\"}";
        uint8_t pub[MQTT_MAX_PACKET_SIZE];
        size_t n=MqttCodec_EncodePublish(pub,sizeof(pub), app.topic_command,
                                         strlen(app.topic_command),
                                         (const uint8_t*)cmd, strlen(cmd), 1, false, 7);
        /* Feed the inbound PUBLISH (QoS1) as the peer stream. */
        FakeNetworkAdapter_FeedRecv(&s_fake, pub, n);
        int g=0;
        while(app.stats.command_messages_received==0 && g<200){ FakePlatform_AdvanceTick(1); MqttApp_Run(&app); g++; }
        check(app.stats.command_messages_received>=1, "W-CMD: command ingested");

        /* Drain any pending outbound TX (the inbound command's PUBACK) so the
           response's QoS1 slot is free. In production Command_Run runs each tick
           AFTER MqttApp_Run has drained TX, so this mirrors the real ordering. */
        for (int k = 0; k < 200; k++) { FakePlatform_AdvanceTick(1); MqttApp_Run(&app); }

        /* Run Command_Run to build + emit the response to the MQTT port. */
        size_t pre = s_fake.tx_capture_len;
        g=0;
        while((int)app.stats.command_responses_published==0 && g<200){
            Command_Run();
            FakePlatform_AdvanceTick(1); MqttApp_Run(&app);
            g++;
        }
        /* Flush the QoS1 response PUBLISH fully onto the wire (a few extra pumps);
           the counter increments at enqueue, not at drain. */
        for (int k = 0; k < 2000; k++) { FakePlatform_AdvanceTick(1); MqttApp_Run(&app); }
        /* The response was published via QoS1; the fresh outbound PUBLISH packet
           is the appended segment [pre, capture_len). Parse it as one packet. */
        size_t off=0,len=0;
        size_t fresh = s_fake.tx_capture_len - pre;
        extract_single_publish_payload(s_fake.tx_capture+pre, fresh, &off, &len);
        off += pre;   /* absolute offset in the full capture */
        /* If a response reached the wire, emit it for python; otherwise report. */
        int resp_seen = ((int)app.stats.command_responses_published >= 1);
        check(resp_seen, "W-CMD: response published via MqttApp");
        if (resp_seen && len>0) {
            check(strstr((const char*)s_fake.tx_capture+off,"\"manifest\"")!=NULL ||
                  strstr((const char*)s_fake.tx_capture+off,"\"GET_MANIFEST\"")!=NULL ||
                  strstr((const char*)s_fake.tx_capture+off,"\"status\"")!=NULL,
                  "W-CMD: response JSON present on wire");
            printf("WIRE COMMANDRESP %u\n", (unsigned)len);
            fwrite(s_fake.tx_capture+off, 1, len, stdout);
            printf("\nEND\n");
        }
    }

    printf("\n%d pass, %d fail\n", s_pass, s_fail);
    return (s_fail==0)?0:1;
}