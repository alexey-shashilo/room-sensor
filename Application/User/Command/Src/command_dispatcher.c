#include "command_dispatcher.h"
#include "command_response.h"
#include "config.h"
#include "device_identity.h"
#include "device_manifest.h"
#include "device_manifest_serializer.h"
#include "provisioning.h"
#include "self_test.h"
#include "i2c_bus.h"
#include "telemetry.h"
#include <string.h>

static void HandleGetStatus(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJsonInt(rsp, "uptime_ms", svc->uptime_ms);
    CommandResponse_AppendJsonInt(rsp, "reset", (uint32_t)svc->reset_cause);
    CommandResponse_Append(rsp, "\"light\":{");
    CommandResponse_AppendJsonInt(rsp, "state", (uint32_t)svc->light_sensor.state);
    CommandResponse_AppendJsonInt(rsp, "ops", svc->light_sensor.operation_successes);
    CommandResponse_AppendJsonInt(rsp, "err", svc->light_sensor.operation_failures);
    CommandResponse_Append(rsp, "},");
    CommandResponse_Append(rsp, "\"display\":{");
    CommandResponse_AppendJsonInt(rsp, "state", (uint32_t)svc->display.state);
    CommandResponse_Append(rsp, "},");
    CommandResponse_Append(rsp, "\"co2_sensor\":{");
    CommandResponse_AppendJsonInt(rsp, "state", (uint32_t)svc->co2_sensor.state);
    CommandResponse_AppendJsonInt(rsp, "ops", svc->co2_sensor.operation_successes);
    CommandResponse_AppendJsonInt(rsp, "err", svc->co2_sensor.operation_failures);
    CommandResponse_AppendJsonInt(rsp, "consec", svc->co2_sensor.consecutive_errors);
    CommandResponse_AppendJsonInt(rsp, "rec", svc->co2_sensor.recovery_count);
    CommandResponse_Append(rsp, "},");
    CommandResponse_Append(rsp, "\"room\":{");
    CommandResponse_AppendJsonFloat(rsp, "illuminance_lux", svc->room->illuminance_lux, 1);
    CommandResponse_AppendJson(rsp, "illuminance", svc->room->illuminance_valid ? "valid" : "invalid");
    CommandResponse_AppendJsonFloat(rsp, "co2_ppm", svc->room->co2_ppm, 0);
    CommandResponse_AppendJson(rsp, "co2", svc->room->co2_valid ? "valid" : "invalid");
    CommandResponse_Append(rsp, "},");
    CommandResponse_AppendJsonBool(rsp, "watchdog", svc->watchdog_active);

    /* Authoritative config/identity/provisioning health comes from the live
       CommandRuntimeStatus snapshot (svc->runtime_status filled by App), NOT
       from the SelfTest report (which only records what a previous diagnostic
       observed). Read status and redundancy health are exposed separately so a
       readable VALID+IO record (read OK, mirror DEGRADED_IO) is not reported as
       fully healthy. */
    const CommandRuntimeStatus *st = svc->runtime_status;
    bool storage_ok = (st != NULL) && st->storage_initialized;
    CommandResponse_AppendJsonBool(rsp, "storage_ok", storage_ok);
    if (st != NULL)
    {
        CommandResponse_AppendJsonInt(rsp, "system_health", (uint32_t)st->system_health);
        CommandResponse_AppendJsonBool(rsp, "storage_initialized", st->storage_initialized);

        /* Config */
        CommandResponse_AppendJsonBool(rsp, "config_ok",
                                       (st->config_persistence != STORAGE_READ_CORRUPT) &&
                                       (st->config_persistence != STORAGE_READ_IO_ERROR));
        CommandResponse_AppendJsonInt(rsp, "config_persistence_status",
                                      (uint32_t)st->config_persistence);
        CommandResponse_AppendJsonInt(rsp, "config_storage_health",
                                      (uint32_t)st->config_storage_health);

        /* Identity */
        CommandResponse_AppendJsonBool(rsp, "identity_ok",
                                       (st->identity_persistence != STORAGE_READ_CORRUPT) &&
                                       (st->identity_persistence != STORAGE_READ_IO_ERROR));
        CommandResponse_AppendJsonInt(rsp, "identity_persistence_status",
                                      (uint32_t)st->identity_persistence);
        CommandResponse_AppendJsonInt(rsp, "identity_storage_health",
                                      (uint32_t)st->identity_storage_health);

        /* Registration / provisioning */
        CommandResponse_AppendJsonBool(rsp, "provisioning_healthy",
                                       st->provisioning_persistence != STORAGE_READ_CORRUPT &&
                                       st->provisioning_persistence != STORAGE_READ_IO_ERROR);
        CommandResponse_AppendJsonInt(rsp, "provisioning_state", (uint32_t)st->provisioning_state);
        CommandResponse_AppendJsonInt(rsp, "provisioning_persistence_status",
                                      (uint32_t)st->provisioning_persistence);
        CommandResponse_AppendJsonInt(rsp, "provisioning_storage_health",
                                      (uint32_t)st->provisioning_storage_health);
    }
    CommandResponse_Finalize(rsp);
}

static void HandleGetConfig(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJsonInt(rsp, "light_period_ms", svc->config->storage.light_period_ms);
    CommandResponse_AppendJsonInt(rsp, "display_period_ms", svc->config->storage.display_period_ms);
    CommandResponse_AppendJsonInt(rsp, "diagnostics_period_ms", svc->config->storage.diagnostics_period_ms);
    CommandResponse_AppendJsonInt(rsp, "retry_period_ms", svc->config->storage.retry_period_ms);
    CommandResponse_AppendJsonInt(rsp, "telemetry_period_ms", svc->config->storage.telemetry_period_ms);
    CommandResponse_AppendJsonFloat(rsp, "light_calibration", svc->config->runtime.light_calibration_factor, 4);
    CommandResponse_Finalize(rsp);
}

static void HandleSetConfig(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    RoomSensorConfig cfg = *svc->config;

    if (req->args.has_light_period_ms)
        cfg.storage.light_period_ms = req->args.light_period_ms;
    if (req->args.has_display_period_ms)
        cfg.storage.display_period_ms = req->args.display_period_ms;
    if (req->args.has_telemetry_period_ms)
        cfg.storage.telemetry_period_ms = req->args.telemetry_period_ms;
    if (req->args.has_light_calibration)
        cfg.runtime.light_calibration_factor = req->args.light_calibration;

    ConfigApplyStatus as = Config_ApplyPersistent(&cfg);
    if (as == CONFIG_APPLY_OK)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
        CommandResponse_AppendJson(rsp, "result", "saved");
    }
    else if (as == CONFIG_APPLY_INVALID)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(rsp, "\"error\":\"invalid_configuration\"");
    }
    else
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"persist_failed\"");
    }
    CommandResponse_Finalize(rsp);
}

static void HandleResetConfig(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    if (Config_ResetToDefaults())
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
        CommandResponse_AppendJson(rsp, "result", "defaults_restored");
    }
    else
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"persist_failed\"");
    }
    CommandResponse_Finalize(rsp);
}

static void HandleGetIdentity(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    char id_str[64];
    DeviceIdentity_GetShortId((const DeviceIdentity *)svc->identity, id_str, sizeof(id_str));
    CommandResponse_AppendJson(rsp, "device_uuid_short", id_str);
    CommandResponse_AppendJsonInt(rsp, "hardware_revision", ((const DeviceIdentity *)svc->identity)->hardware_revision);
    CommandResponse_Finalize(rsp);
}

static void HandleSelfTest(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    if (svc->bus && svc->self_test)
    {
        SelfTestReport report;
        SelfTest_Run(&report, (const I2cBus *)svc->bus);
        *svc->self_test = report;
    }
    const SelfTestReport *st = svc->self_test;
    SelfTestReport empty_report;
    if (st == NULL) { memset(&empty_report, 0, sizeof(empty_report)); st = &empty_report; }
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    /* Report every SelfTest field, preserving distinct states (degraded /
       skipped / fail are NOT collapsed into a generic pass/fail). */
    CommandResponse_AppendJson(rsp, "platform",
                               SelfTestResult_ToProtocolString(st->platform));
    CommandResponse_AppendJson(rsp, "i2c",
                               SelfTestResult_ToProtocolString(st->i2c));
    CommandResponse_AppendJson(rsp, "storage",
                               SelfTestResult_ToProtocolString(st->storage));
    CommandResponse_AppendJson(rsp, "config",
                               SelfTestResult_ToProtocolString(st->config));
    CommandResponse_AppendJson(rsp, "identity",
                               SelfTestResult_ToProtocolString(st->identity));
    CommandResponse_AppendJson(rsp, "light_sensor",
                               SelfTestResult_ToProtocolString(st->light_sensor));
    CommandResponse_AppendJson(rsp, "display",
                               SelfTestResult_ToProtocolString(st->display));
    CommandResponse_AppendJson(rsp, "co2_sensor",
                               SelfTestResult_ToProtocolString(st->co2_sensor));
    CommandResponse_AppendJson(rsp, "temp_humidity_sensor",
                               SelfTestResult_ToProtocolString(st->temp_humidity_sensor));
    CommandResponse_AppendJson(rsp, "pressure_sensor",
                               SelfTestResult_ToProtocolString(st->pressure_sensor));
    CommandResponse_AppendJson(rsp, "air_quality_sensor",
                               SelfTestResult_ToProtocolString(st->air_quality_sensor));
    CommandResponse_Finalize(rsp);
}

static void HandleReboot(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_NOT_SUPPORTED);
    CommandResponse_Append(rsp, "\"message\":\"REBOOT_disabled\"");
    CommandResponse_Finalize(rsp);
}

static void HandleGetCapabilities(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    /* P1-4 / P2-7: derive EVERY capability from the single canonical
       DeviceCapabilities_Get() source instead of a second hand-maintained table.
       This eliminates the pressure=true (canonical) / pressure=false
       (GET_CAPABILITIES) drift and lets NOx / presence / display /
       persistent_config / telemetry / command_control / watchdog / self_test all
       come from one place, so they cannot silently drift from the manifest. */
    DeviceCapabilities caps;
    DeviceCapabilities_Get(&caps);

    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJsonInt(rsp, "command_schema", COMMAND_SCHEMA_VERSION);
    CommandResponse_AppendJsonInt(rsp, "telemetry_schema", TELEMETRY_SCHEMA_VERSION);
    CommandResponse_AppendJsonInt(rsp, "config_schema", CONFIG_SCHEMA_VERSION);
    CommandResponse_AppendJsonBool(rsp, "illuminance", caps.illuminance);
    CommandResponse_AppendJsonBool(rsp, "temperature", caps.temperature);
    CommandResponse_AppendJsonBool(rsp, "humidity", caps.relative_humidity);
    CommandResponse_AppendJsonBool(rsp, "pressure", caps.pressure);
    CommandResponse_AppendJsonBool(rsp, "co2", caps.co2);
    CommandResponse_AppendJsonBool(rsp, "voc", caps.voc);
    CommandResponse_AppendJsonBool(rsp, "nox", caps.nox);
    CommandResponse_AppendJsonBool(rsp, "presence", caps.presence);
    CommandResponse_Finalize(rsp);
}

static void HandleGetManifest(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    /* SECURITY (P1-3): GET_MANIFEST is READ_ONLY and may be served to an
       untrusted source. The manifest is BUILT from the authoritative runtime
       identity owned by App (svc->identity), never by re-reading persistence.
       DeviceManifest_Build fully initializes every field, so no uninitialized
       bytes can ever be serialized even when identity storage is CORRUPT /
       IO_ERROR / NOT_FOUND. */
    DeviceManifest manifest;
    if (svc != NULL)
        DeviceManifest_Build(&manifest, svc->identity);
    else
        DeviceManifest_Build(&manifest, NULL);

    uint8_t buf[DEVICE_MANIFEST_SERIALIZED_MAX_SIZE];
    size_t written = 0;
    ManifestSerializeStatus ms = DeviceManifest_Serialize(&manifest, buf, sizeof(buf), &written);

    if (ms != MANIFEST_SERIALIZE_OK || written == 0)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"serialization_failed\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    if (!CommandResponse_AppendJsonRaw(rsp, "manifest", buf, written) || rsp->overflowed)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"response_too_large\"");
    }
    CommandResponse_Finalize(rsp);
}

static void HandleGetProvisioningStatus(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    const ProvisioningRuntime *rt = Provisioning_GetRuntime();
    const ProvisioningStatus *ps = &rt->status;

    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJsonInt(rsp, "state", (uint32_t)ps->state);
    CommandResponse_AppendJsonBool(rsp, "registered", ps->registered);
    CommandResponse_AppendJsonBool(rsp, "installation_valid", ps->installation_valid);
    CommandResponse_AppendJsonBool(rsp, "building_valid", ps->building_valid);
    CommandResponse_AppendJsonBool(rsp, "room_valid", ps->room_valid);
    CommandResponse_AppendJsonInt(rsp, "revision", ps->revision);
    CommandResponse_AppendJsonInt(rsp, "storage_status", (uint32_t)rt->storage_status);
    CommandResponse_Finalize(rsp);
}

static void HandleRegisterDevice(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    if (!req->args.has_installation_id)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(rsp, "\"error\":\"missing_installation_id\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    EntityId inst_id;
    memcpy(inst_id.bytes, req->args.installation_id, ENTITY_ID_SIZE);
    if (!Provisioning_ValidEntityId(&inst_id))
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(rsp, "\"error\":\"invalid_installation_id\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    const ProvisioningRuntime *rt = Provisioning_GetRuntime();
    const DeviceRegistration *current = &rt->current;

    if (rt->storage_status != STORAGE_READ_OK &&
        rt->storage_status != STORAGE_READ_NOT_FOUND)
    {
        /* Registration storage state is unknown/error — ownership mutations
           must fail (fail closed) until storage is recovered. */
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"storage_state_unknown\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    if (current->registered && current->installation_valid)
    {
        if (memcmp(current->installation_id.bytes, req->args.installation_id, 16) != 0)
        {
            CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_CONFLICT);
            CommandResponse_Append(rsp, "\"error\":\"ownership_conflict\"");
            CommandResponse_Finalize(rsp);
            return;
        }
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
        CommandResponse_AppendJson(rsp, "result", "already_registered");
        CommandResponse_Finalize(rsp);
        return;
    }

    DeviceRegistration updated = *current;
    updated.registered = true;
    memcpy(updated.installation_id.bytes, req->args.installation_id, 16);
    updated.installation_valid = true;
    /* building/room are cleared by the canonicalizer on save. */
    memset(updated.building_id.bytes, 0, ENTITY_ID_SIZE);
    memset(updated.room_id.bytes, 0, ENTITY_ID_SIZE);
    updated.building_valid = false;
    updated.room_valid = false;

    if (!Provisioning_Save(&updated))
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"persist_failed\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJson(rsp, "result", "registered");
    CommandResponse_Finalize(rsp);
}

static void HandleUnregisterDevice(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    if (!Provisioning_Clear())
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"unregister_failed\"");
        CommandResponse_Finalize(rsp);
        return;
    }
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJson(rsp, "result", "unregistered");
    CommandResponse_Finalize(rsp);
}

static void HandleFactoryReset(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    bool reg_cleared = Provisioning_Clear();
    bool cfg_reset = false;
    if (reg_cleared)
        cfg_reset = Config_ResetToDefaults();

    if (reg_cleared && cfg_reset)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
        CommandResponse_AppendJson(rsp, "result", "factory_reset_complete");
    }
    else
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"factory_reset_incomplete\"");
    }
    CommandResponse_Finalize(rsp);
}

static void HandleAssignLocation(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    if (!req->args.has_installation_id || !req->args.has_building_id || !req->args.has_room_id)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(rsp, "\"error\":\"missing_required_ids\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    EntityId inst_id, bld_id, room_id;
    memcpy(inst_id.bytes, req->args.installation_id, ENTITY_ID_SIZE);
    memcpy(bld_id.bytes, req->args.building_id, ENTITY_ID_SIZE);
    memcpy(room_id.bytes, req->args.room_id, ENTITY_ID_SIZE);

    /* Reject zero / all-FF IDs (invalid domain values). */
    if (!Provisioning_ValidEntityId(&inst_id) ||
        !Provisioning_ValidEntityId(&bld_id) ||
        !Provisioning_ValidEntityId(&room_id))
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(rsp, "\"error\":\"invalid_entity_id\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    const ProvisioningRuntime *rt = Provisioning_GetRuntime();
    const DeviceRegistration *current = &rt->current;

    if (rt->storage_status != STORAGE_READ_OK &&
        rt->storage_status != STORAGE_READ_NOT_FOUND)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"storage_state_unknown\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    if (!current->registered || !current->installation_valid)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(rsp, "\"error\":\"device_not_registered\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    /* Wrong owner: installation mismatch is a CONFLICT. */
    if (memcmp(current->installation_id.bytes, req->args.installation_id, 16) != 0)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_CONFLICT);
        CommandResponse_Append(rsp, "\"error\":\"installation_mismatch\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    if (current->building_valid && current->room_valid &&
        memcmp(current->building_id.bytes, req->args.building_id, 16) == 0 &&
        memcmp(current->room_id.bytes, req->args.room_id, 16) == 0)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
        CommandResponse_AppendJson(rsp, "result", "already_assigned");
        CommandResponse_Finalize(rsp);
        return;
    }

    DeviceRegistration updated = *current;
    memcpy(updated.building_id.bytes, req->args.building_id, 16);
    memcpy(updated.room_id.bytes, req->args.room_id, 16);
    updated.building_valid = true;
    updated.room_valid = true;

    if (!Provisioning_Save(&updated))
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"persist_failed\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJson(rsp, "result", "location_assigned");
    CommandResponse_Finalize(rsp);
}

static void HandleUnknown(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_COMMAND);
    CommandResponse_Append(rsp, "\"error\":\"unknown_command\"");
    CommandResponse_Finalize(rsp);
}

bool CommandDispatcher_Dispatch(
    const CommandRequest *request,
    CommandResponse *response,
    const CommandServices *services)
{
    if ((request == NULL) || (response == NULL) || (services == NULL))
        return false;

    switch (request->type)
    {
        case COMMAND_GET_STATUS:              HandleGetStatus(request, response, services); break;
        case COMMAND_GET_CONFIG:              HandleGetConfig(request, response, services); break;
        case COMMAND_SET_CONFIG:              HandleSetConfig(request, response, services); break;
        case COMMAND_RESET_CONFIG:            HandleResetConfig(request, response, services); break;
        case COMMAND_GET_IDENTITY:            HandleGetIdentity(request, response, services); break;
        case COMMAND_SELF_TEST:               HandleSelfTest(request, response, services); break;
        case COMMAND_REBOOT:                  HandleReboot(request, response, services); break;
        case COMMAND_GET_CAPABILITIES:        HandleGetCapabilities(request, response, services); break;
        case COMMAND_GET_MANIFEST:            HandleGetManifest(request, response, services); break;
        case COMMAND_REGISTER_DEVICE:         HandleRegisterDevice(request, response, services); break;
        case COMMAND_UNREGISTER_DEVICE:       HandleUnregisterDevice(request, response, services); break;
        case COMMAND_FACTORY_RESET:           HandleFactoryReset(request, response, services); break;
        case COMMAND_GET_PROVISIONING_STATUS: HandleGetProvisioningStatus(request, response, services); break;
        case COMMAND_ASSIGN_LOCATION:         HandleAssignLocation(request, response, services); break;
        default:                              HandleUnknown(request, response, services); break;
    }
    return true;
}