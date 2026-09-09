const MODULE = "mhcamera";

const ROUTES = Object.freeze({
  authStatus: "/auth/status",
  authSmsStart: "/auth/sms/start",
  authSmsVerify: "/auth/sms/verify",
  authSmsResend: "/auth/sms/resend",
  authCancel: "/auth/cancel",
  authClear: "/auth/clear",
  cameraList: "/camera/list",
  cameraRegion: "/camera/region",
  cameraSelect: "/camera/select",
  cameraStop: "/camera/stop",
  cameraStatus: "/camera/status",
  rtsp: "/rtsp",
  rtspCredentials: "/rtsp/credentials"
});

const AuthState = Object.freeze({
  IDLE: 0,
  WORKING: 1,
  SMS_REQUIRED: 2,
  AUTHENTICATED: 3,
  REAUTH_REQUIRED: 4,
  ERROR: 5,
  RETRYING: 6
});

const AuthReason = Object.freeze({
  NONE: 0,
  SMS_START: 1,
  SMS_VERIFY: 2,
  CLOUD_SESSION_REJECTED: 3,
  PASS_TOKEN_REJECTED: 4,
  AUTH_CLEAR: 5
});

const CatalogState = Object.freeze({
  IDLE: 0,
  QUEUED: 1,
  LOADING: 2,
  READY: 3,
  ERROR: 4,
  RETRYING: 5
});

const MediaState = Object.freeze({
  STOPPED: 0,
  STARTING: 1,
  RUNNING: 2,
  WAITING_INPUT: 3,
  ERROR: 4
});

const RTSPState = Object.freeze({
  DISABLED: "disabled",
  WAITING_MEDIA: "waiting_media",
  RUNNING: "running",
  ERROR: "error"
});

const SourceState = Object.freeze({
  STOPPED: "stopped",
  STARTING: "starting",
  RUNNING: "running",
  RETRYING: "retrying",
  STOPPING: "stopping",
  ERROR: "error"
});

const RequestStage = Object.freeze({ UNKNOWN: 0, TOKEN_LOGIN: 1, TOKEN_FINISH: 2, CLOUD_API: 3, LOCAL_RPC: 4 });
const NetworkReason = Object.freeze({ UNKNOWN: 0, DNS: 1, CONNECT: 2, TIMEOUT: 3, TLS_CERTIFICATE: 4, TLS_HANDSHAKE: 5, CANCELLED: 6, IO: 7 });

const CameraRegion = Object.freeze({
  CN: "cn",
  DE: "de",
  I2: "i2",
  RU: "ru",
  SG: "sg",
  US: "us"
});

const CAMERA_REGIONS = Object.freeze(Object.values(CameraRegion));
const AUTH_STATES = new Set(Object.values(AuthState));
const AUTH_REASONS = new Set(Object.values(AuthReason));
const CATALOG_STATES = new Set(Object.values(CatalogState));
const MEDIA_STATES = new Set(Object.values(MediaState));
const RTSP_STATES = new Set(Object.values(RTSPState));
const SOURCE_STATES = new Set(Object.values(SourceState));
const REQUEST_STAGES = new Set(Object.values(RequestStage));
const NETWORK_REASONS = new Set(Object.values(NetworkReason));
const CAMERA_REGION_SET = new Set(CAMERA_REGIONS);

function isRecord(value) {
  return Boolean(value) && typeof value === "object" && !Array.isArray(value);
}

function stableErrorProjection(value, fallbackStatus = 0) {
  const envelope = isRecord(value?.error) ? value.error : value;
  const details = isRecord(envelope?.details) ? envelope.details : {};
  const providerCode = Number.isSafeInteger(envelope?.provider_code)
    ? envelope.provider_code
    : Number.isSafeInteger(details.provider_code)
      ? details.provider_code
      : null;
  const category = typeof envelope?.category === "string"
    ? envelope.category
    : typeof details.category === "string"
      ? details.category
      : "internal";
  const messageKey = typeof envelope?.message_key === "string"
    ? envelope.message_key
    : typeof details.message_key === "string"
      ? details.message_key
      : typeof details.reason === "string"
        ? details.reason
        : "request_failed";
  const status = Number.isSafeInteger(envelope?.status)
    ? envelope.status
    : Number.isSafeInteger(envelope?.http_status)
      ? envelope.http_status
      : Number.isSafeInteger(details.http_status)
        ? details.http_status
        : fallbackStatus;
  return Object.freeze({ category, provider_code: providerCode, message_key: messageKey, status });
}

class CameraRouteError extends Error {
  constructor(projection) {
    super("mhcamera route failed");
    this.name = "CameraRouteError";
    this.projection = projection;
  }
}

function protocolError() {
  return new CameraRouteError(Object.freeze({
    category: "protocol", provider_code: null, message_key: "request_failed", status: 0
  }));
}

function normalizeThrownError(error) {
  if (error instanceof CameraRouteError) return error;
  const candidate = isRecord(error?.body) ? error.body : isRecord(error?.details) ? error.details : {};
  return new CameraRouteError(stableErrorProjection(candidate, Number(error?.status) || 0));
}

function requireString(value, name) {
  if (typeof value !== "string" || value.length === 0) {
    throw new TypeError(`${name} must be a non-empty string`);
  }
  return value;
}

function requireChannel(value) {
  if (!Number.isSafeInteger(value) || (value !== 1 && value !== 2)) {
    throw new TypeError("channel must be 1 or 2");
  }
  return value;
}

function requireRegion(value) {
  if (typeof value !== "string" || !CAMERA_REGION_SET.has(value)) {
    throw new TypeError("region is invalid");
  }
  return value;
}

function requireEnum(value, allowed) {
  if (!Number.isSafeInteger(value) || !allowed.has(value)) throw protocolError();
  return value;
}

function authDisplayState(value) {
  if (!isRecord(value)) throw new TypeError("auth projection is required");
  return value.state_code === AuthState.ERROR && value.account === null
    ? AuthState.IDLE
    : value.state_code;
}

function authInteractionPolicy(stateCode, localAttemptActive) {
  const state = requireEnum(stateCode, AUTH_STATES);
  return Object.freeze({
    attemptActive: Boolean(localAttemptActive) || state === AuthState.SMS_REQUIRED,
    phoneLocked: [AuthState.WORKING, AuthState.SMS_REQUIRED, AuthState.RETRYING].includes(state)
  });
}

function reauthNoticeTransition(stateCode, wasShown) {
  const active = requireEnum(stateCode, AUTH_STATES) === AuthState.REAUTH_REQUIRED;
  return Object.freeze({ show: active && !wasShown, shown: active });
}

function authPollError(projection) {
  if (!isRecord(projection)) throw new TypeError("auth projection is required");
  return requireEnum(projection.state_code, AUTH_STATES) === AuthState.ERROR
    ? projection.last_error || Object.freeze({ message_key: "request_failed" })
    : null;
}

function normalizeOperationResult(result, operationId) {
  if (
    !isRecord(result?.operation) ||
    result.operation.id !== operationId ||
    result.operation.state !== "working"
  ) {
    throw protocolError();
  }
  return Object.freeze({
    operation: Object.freeze({ id: result.operation.id, state: "working" })
  });
}

function clearBodySecrets(body, keys) {
  for (const key of keys) {
    if (Object.prototype.hasOwnProperty.call(body, key)) body[key] = "";
  }
}

function createCameraApi(bridge) {
  if (!bridge || typeof bridge.request !== "function") {
    throw new TypeError("bridge client is required");
  }

  async function route(path, httpMethod = "GET", query = {}, body = {}) {
    try {
      const result = await bridge.request({
        module: MODULE,
        method: "route",
        params: { path, http_method: httpMethod, query, body }
      }, { queueKey: MODULE });
      // RTSP status owns a domain error field; it is still a successful state projection.
      const rtspProjection = path === ROUTES.rtsp && typeof result?.enabled === "boolean";
      if (!rtspProjection && isRecord(result?.error)) throw new CameraRouteError(stableErrorProjection(result));
      return result;
    } catch (error) {
      throw normalizeThrownError(error);
    }
  }

  function mutate(path, operationId, fields = {}, secretKeys = []) {
    const validatedOperationId = requireString(operationId, "operation_id");
    const body = { operation_id: validatedOperationId, ...fields };
    return route(path, "POST", {}, body)
      .then((result) => normalizeOperationResult(result, validatedOperationId))
      .finally(() => clearBodySecrets(body, secretKeys));
  }

  return Object.freeze({
    authStatus: () => route(ROUTES.authStatus),
    startSms: (operationId, callingCode, nationalNumber) => mutate(
      ROUTES.authSmsStart,
      operationId,
      {
        calling_code: requireString(callingCode, "calling_code"),
        national_number: requireString(nationalNumber, "national_number")
      },
      ["calling_code", "national_number"]
    ),
    verifySms: (operationId, code) => mutate(
      ROUTES.authSmsVerify,
      operationId,
      { code: requireString(code, "code") },
      ["code"]
    ),
    resendSms: (operationId) => mutate(ROUTES.authSmsResend, operationId),
    cancelAuthorization: (operationId) => mutate(ROUTES.authCancel, operationId),
    clearAuthorization: (operationId) => mutate(ROUTES.authClear, operationId),
    cameraList: (refresh = false) => {
      if (typeof refresh !== "boolean") throw new TypeError("refresh must be a boolean");
      const query = {};
      if (refresh) query.refresh = "true";
      return route(ROUTES.cameraList, "GET", query);
    },
    setCameraRegion: (operationId, region) => mutate(
      ROUTES.cameraRegion,
      operationId,
      { region: requireRegion(region) }
    ),
    selectCamera: (operationId, region, cameraId, channel) => {
      const selectedChannel = requireChannel(channel);
      const fields = {
        region: requireRegion(region),
        camera_id: requireString(cameraId, "camera_id")
      };
      if (selectedChannel === 2) fields.channel = 2;
      return mutate(ROUTES.cameraSelect, operationId, fields);
    },
    stopCamera: (operationId) => mutate(ROUTES.cameraStop, operationId),
    cameraStatus: () => route(ROUTES.cameraStatus),
    rtspStatus: () => route(ROUTES.rtsp).then(normalizeRtspResult),
    setRtspEnabled: (enabled) => {
      if (typeof enabled !== "boolean") throw new TypeError("enabled must be a boolean");
      return route(ROUTES.rtsp, "POST", {}, { enabled }).then(normalizeRtspMutationResult);
    },
    setRtspAudioEnabled: (audio_enabled) => {
      if (typeof audio_enabled !== "boolean") throw new TypeError("audio_enabled must be a boolean");
      return route(ROUTES.rtsp, "POST", {}, { audio_enabled }).then(normalizeRtspMutationResult);
    },
    rtspCredentials: () => route(ROUTES.rtspCredentials).then(normalizeRtspCredentials),
    rotateRtspCredentials: () => route(ROUTES.rtspCredentials, "POST", {}, {}).then(normalizeRtspCredentials)
  });
}

function normalizeRtspMutationResult(result) {
  const state = normalizeRtspResult(result);
  if (typeof result.reconnect_requested !== "boolean") throw protocolError();
  return Object.freeze({ ...state, reconnect_requested: result.reconnect_requested });
}

function normalizeRtspCredentials(result) {
  if (!isRecord(result) || result.username !== "viewer" || typeof result.password !== "string" || !result.password) throw protocolError();
  return { username: result.username, password: result.password };
}

function normalizeRtspResult(value) {
  if (!isRecord(value) || typeof value.enabled !== "boolean" || typeof value.audio_enabled !== "boolean" || typeof value.audio_supported !== "boolean" || typeof value.running !== "boolean" ||
      !RTSP_STATES.has(value.state) || !SOURCE_STATES.has(value.source_state) ||
      ![null, "source_dial_failed", "source_read_failed", "audio_unavailable", "credential_rejected"].includes(value.source_error) ||
      typeof value.host !== "string" || value.port !== 8554 || typeof value.url !== "string" ||
      value.username !== "viewer" || !(value.error === null || isRecord(value.error))) throw protocolError();
  return Object.freeze({
    enabled: value.enabled, audio_enabled: value.audio_enabled, audio_supported: value.audio_supported, running: value.running, state: value.state,
    source_state: value.source_state, source_error: value.source_error,
    host: value.host, port: value.port, url: value.url, username: value.username,
    error: value.error ? stableErrorProjection(value.error) : null
  });
}

function normalizeAccount(value) {
  if (value === null) return null;
  if (!isRecord(value) || typeof value.masked_id !== "string" || value.masked_id.length === 0) {
    throw protocolError();
  }
  return Object.freeze({ masked_id: value.masked_id });
}

function normalizeChallenge(value) {
  if (value === null) return null;
  if (!isRecord(value)) throw protocolError();
  if (value.kind === "sms") {
    if (
      typeof value.masked_target !== "string" || value.masked_target.length === 0 ||
      !Number.isSafeInteger(value.code_length) || value.code_length <= 0 || value.code_length > 64 ||
      !Number.isSafeInteger(value.retry_after_seconds) || value.retry_after_seconds < 0
    ) {
      throw protocolError();
    }
    return Object.freeze({
      kind: "sms",
      masked_target: value.masked_target,
      code_length: value.code_length,
      retry_after_seconds: value.retry_after_seconds
    });
  }
  throw protocolError();
}

function normalizeStateError(value) {
  if (value === null) return null;
  if (!isRecord(value)) throw protocolError();
  const projection = stableErrorProjection(value);
  if (projection.category !== "network" && value.network_reason_code !== NetworkReason.UNKNOWN) throw protocolError();
  return Object.freeze({
    ...projection,
    request_stage_code: requireEnum(value.request_stage_code, REQUEST_STAGES),
    network_reason_code: requireEnum(value.network_reason_code, NETWORK_REASONS)
  });
}

function normalizeRetry(value, retrying) {
  if (value === null && !retrying) return null;
  if (!retrying) throw protocolError();
  if (!isRecord(value) || !Number.isSafeInteger(value.attempt) || value.attempt < 0 || value.attempt > 0xffffffff ||
      !Number.isSafeInteger(value.delay_ms) || value.delay_ms < 0 || value.delay_ms > 0xffffffff) throw protocolError();
  return Object.freeze({ attempt: value.attempt, delay_ms: value.delay_ms });
}

function normalizeAuthResult(result) {
  const projection = result?.auth;
  if (
    !isRecord(projection) ||
    typeof projection.state_text !== "string" || projection.state_text.length === 0 ||
    typeof projection.reason_text !== "string" || projection.reason_text.length === 0 ||
    (projection.last_error !== null && !isRecord(projection.last_error))
  ) {
    throw protocolError();
  }
  const stateCode = requireEnum(projection.state_code, AUTH_STATES);
  const reasonCode = requireEnum(projection.reason_code, AUTH_REASONS);
  const account = normalizeAccount(projection.account);
  const challenge = normalizeChallenge(projection.challenge);
  const lastError = normalizeStateError(projection.last_error);
  const retry = normalizeRetry(projection.retry, stateCode === AuthState.RETRYING);
  const validReauth = stateCode !== AuthState.REAUTH_REQUIRED || (
    account !== null &&
    challenge === null &&
    reasonCode === AuthReason.PASS_TOKEN_REJECTED &&
    lastError?.category === "authorization" &&
    lastError.message_key === "xiaomi_reauthorization_required"
  );
  if (
    (stateCode === AuthState.SMS_REQUIRED && challenge?.kind !== "sms") ||
    (stateCode !== AuthState.SMS_REQUIRED && challenge !== null) ||
    ([AuthState.AUTHENTICATED, AuthState.REAUTH_REQUIRED].includes(stateCode) && account === null) ||
    (stateCode === AuthState.IDLE && account !== null) ||
    (stateCode === AuthState.ERROR && lastError === null) ||
    (stateCode === AuthState.RETRYING && (reasonCode !== AuthReason.NONE || lastError === null)) ||
    !validReauth
  ) {
    throw protocolError();
  }
  return Object.freeze({
    state_code: stateCode,
    reason_code: reasonCode,
    account,
    challenge,
    last_error: lastError,
    retry
  });
}

function normalizeCameraListResult(result) {
  const projection = result?.camera_list;
  if (
    !isRecord(projection) ||
    typeof projection.state_text !== "string" || projection.state_text.length === 0 ||
    !CAMERA_REGION_SET.has(projection.region) ||
    !(projection.pending_region === null || CAMERA_REGION_SET.has(projection.pending_region)) ||
    !Array.isArray(projection.regions) ||
    projection.regions.length !== CAMERA_REGIONS.length ||
    !projection.regions.every((region, index) => region === CAMERA_REGIONS[index]) ||
    !Array.isArray(projection.items) ||
    (projection.last_error !== null && !isRecord(projection.last_error))
  ) {
    throw protocolError();
  }
  const stateCode = requireEnum(projection.state_code, CATALOG_STATES);
  if (stateCode === CatalogState.RETRYING && projection.last_error === null) throw protocolError();
  const validItems = projection.items.every((camera) => (
    isRecord(camera) &&
    typeof camera.id === "string" && /^[0-9]+$/.test(camera.id) &&
    typeof camera.name === "string" &&
    typeof camera.model === "string" &&
    typeof camera.home_id === "string" && (camera.home_id === "" || /^[0-9]+$/.test(camera.home_id)) &&
    typeof camera.home_name === "string" &&
    typeof camera.room_id === "string" && (camera.room_id === "" || /^[0-9]+$/.test(camera.room_id)) &&
    typeof camera.room_name === "string"
  ));
  if (!validItems) throw protocolError();
  const items = projection.items.map((camera) => Object.freeze({
    id: camera.id,
    name: camera.name,
    model: camera.model,
    home_id: camera.home_id,
    home_name: camera.home_name,
    room_id: camera.room_id,
    room_name: camera.room_name
  }));
  return Object.freeze({
    state_code: stateCode,
    region: projection.region,
    pending_region: projection.pending_region,
    regions: CAMERA_REGIONS,
    items: Object.freeze(items),
    last_error: normalizeStateError(projection.last_error),
    retry: normalizeRetry(projection.retry, stateCode === CatalogState.RETRYING)
  });
}

function normalizeCameraStatusResult(result) {
  const projection = result?.camera;
  if (
    !isRecord(projection) ||
    typeof projection.state_text !== "string" || projection.state_text.length === 0 ||
    ![null, "h264", "h265"].includes(projection.codec) ||
    (projection.last_error !== null && !isRecord(projection.last_error))
  ) {
    throw protocolError();
  }
  const stateCode = requireEnum(projection.state_code, MEDIA_STATES);
  let selected = null;
  if (projection.selected !== null) {
    if (!isRecord(projection.selected)) throw protocolError();
    const hasChannel = Object.prototype.hasOwnProperty.call(projection.selected, "channel");
    if (
      typeof projection.selected.id !== "string" || !/^[0-9]+$/.test(projection.selected.id) ||
      !CAMERA_REGION_SET.has(projection.selected.region) ||
      typeof projection.selected.name !== "string" ||
      typeof projection.selected.model !== "string" ||
      (hasChannel && projection.selected.channel !== 2)
    ) {
      throw protocolError();
    }
    selected = Object.freeze({
      id: projection.selected.id,
      region: projection.selected.region,
      name: projection.selected.name,
      model: projection.selected.model,
      channel: hasChannel ? 2 : 1
    });
  }
  return Object.freeze({
    state_code: stateCode,
    selected,
    codec: projection.codec,
    last_error: normalizeStateError(projection.last_error)
  });
}

function waitForVisibleDocument(documentRef) {
  if (!documentRef || typeof documentRef.addEventListener !== "function") {
    throw new TypeError("document is required");
  }
  if (documentRef.visibilityState !== "hidden") return Promise.resolve();
  return new Promise((resolve) => {
    function handleVisibility() {
      if (documentRef.visibilityState === "hidden") return;
      documentRef.removeEventListener("visibilitychange", handleVisibility);
      resolve();
    }
    documentRef.addEventListener("visibilitychange", handleVisibility);
  });
}

class MutationGate {
  constructor(operationIdFactory) {
    if (typeof operationIdFactory !== "function") {
      throw new TypeError("operation id factory is required");
    }
    this.operationIdFactory = operationIdFactory;
    this.active = new Map();
  }

  run(key, task) {
    requireString(key, "mutation key");
    if (typeof task !== "function") throw new TypeError("mutation task is required");
    const existing = this.active.get(key);
    if (existing) return existing;

    const operationId = requireString(this.operationIdFactory(), "generated operation id");
    let promise;
    promise = Promise.resolve()
      .then(() => task(operationId))
      .finally(() => {
        if (this.active.get(key) === promise) this.active.delete(key);
      });
    this.active.set(key, promise);
    return promise;
  }

  has(key) {
    return this.active.has(key);
  }

  get busy() {
    return this.active.size > 0;
  }
}

class NoticeDeduper {
  constructor() {
    this.signatures = new Map();
  }

  accept(scope, signature) {
    requireString(scope, "notice scope");
    requireString(signature, "notice signature");
    if (this.signatures.get(scope) === signature) return false;
    this.signatures.set(scope, signature);
    return true;
  }

  clear(scope) {
    this.signatures.delete(requireString(scope, "notice scope"));
  }
}

class StatusErrorTracker {
  constructor() {
    this.signatures = new Map();
    this.hydrated = false;
  }

  observe(scope, error) {
    requireString(scope, "status error scope");
    const signature = error ? errorSignature(error) : "";
    const previous = this.signatures.get(scope) || "";
    this.signatures.set(scope, signature);
    return this.hydrated && signature !== "" && signature !== previous;
  }

  finishHydration() {
    this.hydrated = true;
  }
}

function consumeSecret(input, trim = false) {
  if (!input || typeof input.value !== "string") return "";
  const value = trim ? input.value.trim() : input.value;
  input.value = "";
  return value;
}

function errorMessageKey(error) {
  const projection = error instanceof CameraRouteError
    ? error.projection
    : stableErrorProjection(error || {});
  if (projection.provider_code === 70022 || projection.message_key === "sms_send_limit_tomorrow") {
    return "sms_send_limit_tomorrow";
  }
  if ([
    "phone_account_not_found",
    "sms_code_invalid_or_expired",
    "additional_verification_required",
    "xiaomi_token_response_invalid"
  ].includes(projection.message_key)) {
    return projection.message_key;
  }
  if (projection.message_key === "busy" || projection.message_key === "BUSY") return "busy";
  if ([
    "xiaomi_reauthorization_required",
    "invalid_region",
    "catalog_incomplete",
    "catalog_retry_exhausted",
    "selected_camera_unavailable"
  ].includes(projection.message_key)) {
    return projection.message_key;
  }
  if (["rtsp_control_failed", "rtsp_persist_failed", "rtsp_persist_not_durable", "rtsp_rollback_failed", "rtsp_random_failed", "rtsp_audio_unsupported", "rtsp_reconnect_failed", "rtsp_source_failed"].includes(projection.message_key)) return projection.message_key;
  if (["bad_request", "bridge_params_invalid", "route_body_invalid", "route_query_invalid"].includes(projection.message_key)) {
    return "invalidRequest";
  }
  if (projection.category === "input") return "invalidRequest";
  if (projection.category === "network") return "networkError";
  if (projection.category === "provider") return "providerError";
  if (projection.category === "protocol") return "protocolError";
  return "requestFailed";
}

function errorSignature(error) {
  const projection = error instanceof CameraRouteError
    ? error.projection
    : stableErrorProjection(error || {});
  return [projection.category, projection.provider_code ?? "", projection.message_key, projection.status].join(":");
}

export {
  AuthReason,
  AuthState,
  CAMERA_REGIONS,
  CameraRouteError,
  CameraRegion,
  CatalogState,
  MODULE,
  MutationGate,
  NoticeDeduper,
  ROUTES,
  MediaState,
  RTSPState,
  SourceState,
  RequestStage,
  NetworkReason,
  StatusErrorTracker,
  authDisplayState,
  authInteractionPolicy,
  authPollError,
  reauthNoticeTransition,
  consumeSecret,
  createCameraApi,
  errorMessageKey,
  errorSignature,
  normalizeAuthResult,
  normalizeCameraListResult,
  normalizeCameraStatusResult,
  normalizeRtspResult,
  stableErrorProjection,
  waitForVisibleDocument
};
