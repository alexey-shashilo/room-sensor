#include "command_dispatcher.h"
#include "command_response.h"
#include "config.h"
#include "device_identity.h"
#include "device_manifest.h"
#include "device_manifest_serializer.h"
#include "provisioning.h"
#include "self_test.h"
#include "i2c_bus.h"
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
    CommandResponse_Append(rsp, "\"room\":{");
    CommandResponse_AppendJsonFloat(rsp, "illuminance_lux", svc->room->illuminance_lux, 1);
    CommandResponse_AppendJson(rsp, "illuminance", svc->room->illuminance_valid ? "valid" : "invalid");
    CommandResponse_Append(rsp, "},");
    CommandResponse_AppendJsonBool(rsp, "watchdog", svc->watchdog_active);
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
    if (st == NULL) st = &(SelfTestReport){0};
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJson(rsp, "platform", st->platform == SELF_TEST_PASS ? "pass" : "fail");
    CommandResponse_AppendJson(rsp, "i2c", st->i2c == SELF_TEST_PASS ? "pass" : "fail");
    CommandResponse_AppendJson(rsp, "light_sensor", st->light_sensor == SELF_TEST_PASS ? "pass" : "fail");
    CommandResponse_AppendJson(rsp, "display", st->display == SELF_TEST_PASS ? "pass" : "fail");
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
    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJsonInt(rsp, "command_schema", COMMAND_SCHEMA_VERSION);
    CommandResponse_AppendJsonInt(rsp, "telemetry_schema", 1);
    CommandResponse_AppendJsonInt(rsp, "config_schema", 1);
    CommandResponse_AppendJsonBool(rsp, "illuminance", true);
    CommandResponse_AppendJsonBool(rsp, "temperature", false);
    CommandResponse_AppendJsonBool(rsp, "humidity", false);
    CommandResponse_AppendJsonBool(rsp, "pressure", false);
    CommandResponse_AppendJsonBool(rsp, "co2", false);
    CommandResponse_AppendJsonBool(rsp, "voc", false);
    CommandResponse_AppendJsonBool(rsp, "presence", false);
    CommandResponse_Finalize(rsp);
}

static void HandleGetManifest(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    DeviceManifest manifest;
    DeviceManifest_Build(&manifest);

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

    DeviceRegistration current, updated;
    if (!Provisioning_Load(&current))
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"cannot_read_registration\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    if (current.registered && current.installation_valid)
    {
        if (memcmp(current.installation_id.bytes, req->args.installation_id, 16) != 0)
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

    updated = current;
    updated.registered = true;
    memcpy(updated.installation_id.bytes, req->args.installation_id, 16);
    updated.installation_valid = true;

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

static void HandleGetProvisioningStatus(const CommandRequest *req, CommandResponse *rsp, const CommandServices *svc)
{
    (void)svc;
    DeviceRegistration reg;
    ProvisioningStatus ps;

    if (!Provisioning_Load(&reg))
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"cannot_read_registration\"");
        CommandResponse_Finalize(rsp);
        return;
    }
    Provisioning_GetStatus(&reg, &ps);

    CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
    CommandResponse_AppendJsonInt(rsp, "state", (uint32_t)ps.state);
    CommandResponse_AppendJsonBool(rsp, "registered", ps.registered);
    CommandResponse_AppendJsonBool(rsp, "installation_valid", ps.installation_valid);
    CommandResponse_AppendJsonBool(rsp, "building_valid", ps.building_valid);
    CommandResponse_AppendJsonBool(rsp, "room_valid", ps.room_valid);
    CommandResponse_AppendJsonInt(rsp, "revision", ps.revision);
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

    DeviceRegistration current;
    if (!Provisioning_Load(&current))
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INTERNAL_ERROR);
        CommandResponse_Append(rsp, "\"error\":\"cannot_read_registration\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    if (!current.registered || !current.installation_valid)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_INVALID_ARGUMENT);
        CommandResponse_Append(rsp, "\"error\":\"device_not_registered\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    if (memcmp(current.installation_id.bytes, req->args.installation_id, 16) != 0)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_CONFLICT);
        CommandResponse_Append(rsp, "\"error\":\"installation_mismatch\"");
        CommandResponse_Finalize(rsp);
        return;
    }

    if (current.building_valid && current.room_valid &&
        memcmp(current.building_id.bytes, req->args.building_id, 16) == 0 &&
        memcmp(current.room_id.bytes, req->args.room_id, 16) == 0)
    {
        CommandResponse_Init(rsp, req->request_id, COMMAND_STATUS_OK);
        CommandResponse_AppendJson(rsp, "result", "already_assigned");
        CommandResponse_Finalize(rsp);
        return;
    }

    memcpy(current.building_id.bytes, req->args.building_id, 16);
    memcpy(current.room_id.bytes, req->args.room_id, 16);
    current.building_valid = true;
    current.room_valid = true;

    if (!Provisioning_Save(&current))
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