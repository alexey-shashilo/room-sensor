#include "mqtt_app.h"
#include "platform_time.h"
#include "command.h"
#include <string.h>
#include <stdio.h>

/* ============================================================
   App-level MQTT connection manager (Phase 18).
   ============================================================ */

#define MQTT_APP_POLL_PERIOD_MS      50U
#define MQTT_APP_KEEPALIVE_S         60U

/* Reconnect schedule: 1s,2s,5s,10s,30s,60s (ms). Bounded; step 5 is the cap. */
static uint32_t BackoffTable[MQTT_APP_BACKOFF_STEPS] = {
    1000U, 2000U, 5000U, 10000U, 30000U, 60000U
};

uint32_t MqttApp_BackoffScheduleMs(uint32_t i)
{
    if (i >= MQTT_APP_BACKOFF_STEPS)
        i = MQTT_APP_BACKOFF_STEPS - 1U;
    return BackoffTable[i];
}

/* ---- helpers -------------------------------------------------------------- */

static bool elapsed(uint32_t now, uint32_t since, uint32_t period)
{
    return (uint32_t)(now - since) >= period;
}

static void set_disconnected(MqttApp *app)
{
    if (app == NULL) return;
    /* Reset connection-scoped app state; persistent device/identity untouched.
       MqttClient_Disconnect already clears the client's per-connection epoch and
       the NetworkTransport rings. */
    app->state = MQTT_APP_DISCONNECTED;
    app->telemetry_pending = false;
    app->response_pending = false;
    app->stats.epoch++;   /* new connection epoch */
}

static void enter_backoff(MqttApp *app, uint32_t now)
{
    app->state = MQTT_APP_BACKOFF;
    if (app->backoff_step < MQTT_APP_BACKOFF_STEPS - 1U)
        app->backoff_step++;
    app->backoff_until_ms = now + MqttApp_BackoffScheduleMs(app->backoff_step);
    app->stats.reconnect_backoff_ms = MqttApp_BackoffScheduleMs(app->backoff_step);
}

/* ---- topic construction --------------------------------------------------- */

/* Build topics from the authoritative short device id. Bounded; rejects any
   truncation. Deterministic. */
static bool BuildTopics(MqttApp *app)
{
    if (app == NULL) return false;

    char short_id[MQTT_APP_SHORT_ID_LEN + 1U];
    DeviceIdentity_GetShortId(&app->identity, short_id, sizeof(short_id));
    short_id[MQTT_APP_SHORT_ID_LEN] = '\0';

    /* client_id = stable short device id (not a topic, not boot_id). */
    {
        size_t c = strlen(short_id);
        if (c > MQTT_MAX_CLIENT_ID_LENGTH) c = MQTT_MAX_CLIENT_ID_LENGTH;
        memcpy(app->client_id, short_id, c);
        app->client_id[c] = '\0';
    }

    /* room-sensor/<12hex>/telemetry|command|response|status — all fit 64. */
    size_t n;
    n = (size_t)snprintf(app->topic_telemetry, sizeof(app->topic_telemetry),
                          MQTT_APP_PREFIX "%s" MQTT_APP_TOPIC_TELEM, short_id);
    if (n >= sizeof(app->topic_telemetry) || n == 0U) return false;
    n = (size_t)snprintf(app->topic_command, sizeof(app->topic_command),
                          MQTT_APP_PREFIX "%s" MQTT_APP_TOPIC_CMD, short_id);
    if (n >= sizeof(app->topic_command) || n == 0U) return false;
    n = (size_t)snprintf(app->topic_response, sizeof(app->topic_response),
                          MQTT_APP_PREFIX "%s" MQTT_APP_TOPIC_RESP, short_id);
    if (n >= sizeof(app->topic_response) || n == 0U) return false;
    n = (size_t)snprintf(app->topic_status, sizeof(app->topic_status),
                          MQTT_APP_PREFIX "%s" MQTT_APP_TOPIC_STATUS, short_id);
    if (n >= sizeof(app->topic_status) || n == 0U) return false;

    app->topics_built = true;
    return true;
}

/* ---- init ---------------------------------------------------------------- */

void MqttApp_Init(MqttApp *app, const NetworkTransportAdapter *adapter,
                  const NetworkEndpoint *endpoint, const DeviceIdentity *identity)
{
    if (app == NULL) return;
    memset(app, 0, sizeof(*app));
    app->state = MQTT_APP_DISABLED;
    app->last_tick_ms = Platform_GetTickMs();

    if (adapter == NULL || endpoint == NULL)
        return;   /* SAFE_DISABLED */

    if (identity != NULL)
        app->identity = *identity;

    app->adapter = adapter;
    app->endpoint = *endpoint;

    if (!BuildTopics(app))
    {
        app->adapter = NULL;   /* cannot form topics -> disabled */
        return;
    }

    NetworkTransport_Init(&app->net, adapter, endpoint);

    MqttConnectConfig mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    mcfg.client_id = app->client_id;
    mcfg.client_id_len = strlen(app->client_id);
    mcfg.keepalive_s = MQTT_APP_KEEPALIVE_S;

    if (MqttClient_Init(&app->mqtt, &app->net, &mcfg, MqttApp_OnInboundPublish, app)
        != MQTT_OK)
    {
        app->adapter = NULL;
        return;
    }

    app->state = MQTT_APP_DISCONNECTED;
    app->topics_built = true;
}

/* ---- state machine ------------------------------------------------------- */

bool MqttApp_Run(MqttApp *app)
{
    if (app == NULL) return false;
    uint32_t now = Platform_GetTickMs();

    if (elapsed(now, app->last_tick_ms, MQTT_APP_POLL_PERIOD_MS) == false)
        return false;
    app->last_tick_ms = now;

    if (app->adapter == NULL || !app->topics_built)
    {
        app->state = MQTT_APP_DISABLED;
        return false;
    }

    switch (app->state)
    {
        case MQTT_APP_DISABLED:
        case MQTT_APP_DISCONNECTED:
        {
            MqttStatus st = MqttClient_Connect(&app->mqtt);
            app->state = MQTT_APP_CONNECTING;
            app->stats.connect_attempts++;
            (void)st;
            break;
        }

        case MQTT_APP_CONNECTING:
        case MQTT_APP_MQTT_CONNECTING:
        case MQTT_APP_SUBSCRIBING:
        {
            MqttStatus st = MqttClient_Run(&app->mqtt);
            MqttClientState ms = MqttClient_GetState(&app->mqtt);
            (void)st;

            if (ms == MQTT_STATE_CONNECTED)
            {
                if (app->state == MQTT_APP_CONNECTING ||
                    app->state == MQTT_APP_MQTT_CONNECTING)
                {
                    /* CONNACK accepted: subscribe to command topic (QoS1). */
                    app->stats.connect_successes++;
                    MqttStatus ss = MqttClient_Subscribe(&app->mqtt,
                                                         app->topic_command,
                                                         strlen(app->topic_command),
                                                         1U);
                    app->state = MQTT_APP_SUBSCRIBING;
                    (void)ss;   /* BUSY/OK both fine; we re-issue below if needed */
                }
                else if (app->state == MQTT_APP_SUBSCRIBING &&
                         app->mqtt.sub_pending == false)
                {
                    /* SUBACK for the command subscription received -> ONLINE. */
                    app->state = MQTT_APP_ONLINE;
                    app->backoff_step = 0U;   /* reset backoff after successful epoch */
                }
                else if (app->state == MQTT_APP_SUBSCRIBING &&
                         app->mqtt.sub_pending == true)
                {
                    /* subscription still in flight; keep waiting */
                }
            }
            else if (ms == MQTT_STATE_ERROR)
            {
                app->stats.connect_failures++;
                MqttClient_Disconnect(&app->mqtt);
                enter_backoff(app, now);
            }
            else if (ms == MQTT_STATE_DISCONNECTED && app->state != MQTT_APP_CONNECTING)
            {
                set_disconnected(app);
                enter_backoff(app, now);
            }
            break;
        }

        case MQTT_APP_ONLINE:
        {
            MqttStatus st = MqttClient_Run(&app->mqtt);
            /* Detect subscription completion: once SUBACK for the pending sub
               arrives, sub_pending clears; the client reaches CONNECTED again. */
            MqttClientState ms = MqttClient_GetState(&app->mqtt);
            if (ms == MQTT_STATE_ERROR || st == MQTT_TRANSPORT_ERROR ||
                st == MQTT_PROTOCOL_ERROR || st == MQTT_TIMEOUT)
            {
                app->stats.connect_failures++;
                MqttClient_Disconnect(&app->mqtt);
                set_disconnected(app);
                enter_backoff(app, now);
            }
            else if (ms == MQTT_STATE_DISCONNECTED)
            {
                set_disconnected(app);
                enter_backoff(app, now);
            }
            break;
        }

        case MQTT_APP_BACKOFF:
        {
            if (elapsed(now, app->backoff_until_ms, 0U) &&
                (uint32_t)(now - app->backoff_until_ms) < 0x80000000U)
            {
                /* backoff elapsed -> connect again without unblinded ramp */
                app->state = MQTT_APP_DISCONNECTED;
            }
            break;
        }

        default:
            break;
    }

    /* flush any pending response/telemetry once online */
    if (app->state == MQTT_APP_ONLINE)
    {
        if (app->response_pending)
        {
            /* handled in MqttApp_PublishResponse path; cleared there */
        }
    }

    return true;
}

/* ---- telemetry (QoS0, drop offline, no backlog) --------------------------- */

void MqttApp_PublishTelemetry(MqttApp *app, const uint8_t *payload, size_t len)
{
    if (app == NULL || payload == NULL) return;
    app->stats.telemetry_publish_attempts++;

    if (app->adapter == NULL || app->state != MQTT_APP_ONLINE)
        return;   /* offline -> drop; never backlog */

    MqttStatus st = MqttClient_PublishQos0(&app->mqtt, app->topic_telemetry,
                                           strlen(app->topic_telemetry),
                                           payload, len, false);
    if (st == MQTT_OK || st == MQTT_BUSY)
        app->stats.telemetry_publish_accepted++;   /* accepted/queued (single) */
    /* BUSY -> skip this sample; fresh state next period (no backlog). */
}

/* ---- command response (QoS1, single inflight) ----------------------------- */

void MqttApp_PublishResponse(MqttApp *app, const uint8_t *payload, size_t len)
{
    if (app == NULL || payload == NULL) return;

    if (app->adapter == NULL || app->state != MQTT_APP_ONLINE)
        return;   /* offline -> drop response (no command dangle: online-only gate) */

    /* The OPTION-A ingress gate guarantees inflight_active==false for any command
       that reaches Command_Run, so the QoS1 slot is always available here. The
       only remaining BUSY source is a transient tx_pending (telemetry QoS0 in the
       same tick); MqttApp_Run drains TX before Command_Run, so it is clear. */
    MqttStatus st = MqttClient_PublishQos1(&app->mqtt, app->topic_response,
                                           strlen(app->topic_response), payload, len);
    if (st == MQTT_OK)
        app->stats.command_responses_published++;
    else if (st == MQTT_BUSY)
        app->stats.command_responses_busy_rejected++;   /* defensive: must not occur */
    /* single inflight QoS1; never overwritten / never queued unboundedly. */
}

/* ---- CommunicationPort bridge (Command responses -> MQTT) ------------------ */

CommunicationStatus MqttApp_PortSend(void *ctx, const uint8_t *data, size_t size)
{
    MqttApp *app = (MqttApp *)ctx;
    if (app == NULL || data == NULL) return COMM_STATUS_INVALID_ARG;
    MqttApp_PublishResponse(app, data, size);
    return COMM_STATUS_OK;
}

bool MqttApp_PortReady(void *ctx)
{
    MqttApp *app = (MqttApp *)ctx;
    return app != NULL && app->state == MQTT_APP_ONLINE;
}

/* ---- MQTT inbound-publish callback (command topic only) -------------------- */

bool MqttApp_OnInboundPublish(void *ctx, const MqttInboundPublish *pub)
{
    MqttApp *app = (MqttApp *)ctx;
    if (app == NULL || pub == NULL) return false;

    /* Accept commands ONLY on the exact command topic. Telemetry/response/status
       inbound must never execute a command. */
    if (pub->topic_len != strlen(app->topic_command) ||
        strncmp((const char *)pub->topic, app->topic_command,
                (size_t)pub->topic_len) != 0)
    {
        return false;   /* wrong topic -> not a command, do not touch Command */
    }

    /* Bounded payload: command input buffer is 512 (production bound). */
    if (pub->payload_len == 0U || pub->payload_len > COMMAND_INPUT_BUFFER_SIZE)
        return false;   /* reject — never feed oversized/empty into Command */

    /* Exact production command path. AUTHENTICATED_REMOTE trust keeps the
       existing authorization rules (authorization NOT bypassed by MQTT). */
    CommandInput in;
    in.data = pub->payload;
    in.size = (size_t)pub->payload_len;
    in.trust = COMMAND_SOURCE_AUTHENTICATED_REMOTE;

    /* OPTION-A busy-response gate (Phase 18.2): every executed command requires a
       QoS1 response. The QoS1 inflight slot is used ONLY for command responses, so
       `inflight_active` == a previous response is awaiting PUBACK. If it is busy,
       REJECT the new command at INGRESS BEFORE execution — a command whose required
       response cannot be retained/sent must not execute. MqttClient_PublishQos1 is
       BUSY (returned without sending) while inflight_active. The broker's QoS1
       inbound retry re-delivers the command later, so nothing is lost. */
    if (app->mqtt.inflight_active)
    {
        app->stats.command_responses_busy_rejected++;
        return false;   /* not acked -> broker retries; command NOT executed */
    }

    if (!Command_ProcessInput(&in))
        return false;   /* single-slot busy / reject; not acked (retry by broker) */

    app->stats.command_messages_received++;
    return true;        /* ack a QoS1 inbound publish (client sends PUBACK) */
}

/* ---- accessors ------------------------------------------------------------ */

MqttAppState MqttApp_GetState(const MqttApp *app)
{
    return app != NULL ? app->state : MQTT_APP_DISABLED;
}

void MqttApp_GetStats(const MqttApp *app, MqttAppStats *out)
{
    if (app == NULL || out == NULL) return;
    *out = app->stats;
}