import { getBridgeClient } from "/platform/sdk/bridge.js";
import { Locale } from "/platform/sdk/locale.js";
import { Toast } from "/platform/sdk/toast.js";
import { createDialogController } from "./dialog.js";
import {
  COUNTRY_CALLING_CODES,
  DEFAULT_COUNTRY_KEY,
  PhoneInputError,
  callingCodeForCountryKey,
  countryKey,
  normalizePhoneFields,
  normalizeSmsCode
} from "./countries.js";
import {
  AuthState,
  CAMERA_REGIONS,
  CatalogState,
  MediaState,
  RTSPState,
  MutationGate,
  NoticeDeduper,
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
  waitForVisibleDocument
} from "./api.js";

const app = document.getElementById("app");
const api = createCameraApi(getBridgeClient());

const messages = {
  zh: {
    skip: "跳到主要内容",
    title: "米家摄像头",
    subtitle: "接入米家摄像头，支持 AI 分析与 RTSP 转发",
    rtspPanel: "RTSP转发",
    rtspToggle: "启用 RTSP 转发",
    rtspAudio: "转发音频",
    rtspAudioOn: "关闭音频",
    rtspAudioOff: "开启音频",
    rtsp_audio_unsupported: "当前摄像头不支持音频控制",
    rtsp_reconnect_failed: "摄像头连接调整失败，请检查设备状态。",
    rtsp_source_failed: "摄像头连接异常，请检查摄像头和网络。",
    rtspLoading: "正在读取状态…",
    rtspSaved: "RTSP 设置已保存",
    rtspAudioEnabled: "音频已开启",
    rtspAudioDisabled: "音频已关闭",
    rtspReconnectRequested: "{setting}，摄像头正在重新连接；已有 RTSP 连接需要重新连接。",
    rtspReadFailed: "无法读取 RTSP 状态，请检查设备连接。",
    rtspWriteFailed: "设置未能完成，请重试。下方显示设备最后确认的设置。",
    rtspRuntimeFailed: "RTSP 服务启动失败，请检查端口占用或设备状态。",
    rtsp_control_failed: "RTSP 服务未能确认设置，请检查设备状态或端口占用。",
    rtsp_persist_failed: "RTSP 设置保存失败，原设置未变；视频可能已停止，请检查设备状态。",
    rtsp_persist_not_durable: "RTSP 设置已提交，但尚未确认写入存储。重启后可能无法保留，请重试保存。",
    rtsp_rollback_failed: "RTSP 设置失败且无法确认回滚，当前服务状态未知。",
    rtsp_random_failed: "无法生成新的密码，原密码未变，请稍后重试。",
    rtspAddress: "码流地址",
    rtspNoAddress: "等待设备获取局域网 IP",
    rtspCopy: "复制",
    rtspCopied: "码流地址已复制",
    rtspCopyFailed: "复制失败，请选中文本手动复制。",
    rtspSingleClient: "提示：仅支持 1 路连接，建立新连接前请先断开原有连接，或点击刷新图标生成新地址",
    rtspRefreshAddress: "刷新码流地址",
    rtspRotated: "密码已刷新，请重新复制码流地址",
    rtspRotateFailed: "刷新未能确认，请复制当前码流地址重连。",
    rtspCredentialsUnknown: "码流地址读取失败。",
    rtspRetry: "重试读取",
    authStatus: "账号",
    aiStatus: "AI",
    idle: "未授权",
    working: "处理中",
    sms_required: "等待短信验证码",
    authenticated: "已授权",
    reauth_required: "需要重新授权",
    stopped: "未运行",
    starting: "正在启动",
    running: "运行中",
    waiting_input: "等待码流",
    error: "异常",
    authPanel: "账号授权",
    callingCode: "国家或地区",
    nationalNumber: "米家手机号",
    getSmsCode: "获取短信验证码",
    sendingSms: "正在发送…",
    authorizedAccount: "当前授权账号",
    accountRecovering: "正在恢复已保存账号",
    authRetrying: "等待网络重试",
    clearAuthorization: "清除授权",
    clearingAuthorization: "正在清除…",
    cameraPanel: "摄像头",
    catalogRetrying: "正在自动重试摄像头目录",
    catalogNetworkRetrying: "暂时无法连接小米服务，正在自动重试",
    catalogNetworkError: "无法连接小米服务，摄像头目录读取失败",
    cameraRegion: "米家服务区",
    region_cn: "中国大陆",
    region_de: "欧洲",
    region_i2: "印度",
    region_ru: "俄罗斯",
    region_sg: "新加坡及东南亚",
    region_us: "美国",
    camera: "米家摄像头",
    unassignedRoom: "未分配房间",
    channelSelection: "摄像头画面",
    channelOne: "画面 1",
    channelTwo: "画面 2",
    smsCode: "短信验证码",
    verifySms: "验证并授权",
    verifyingSms: "正在验证…",
    resendSms: "重新发送",
    resendingSms: "正在重新发送…",
    resendCountdown: "{seconds} 秒后可重新发送",
    changePhoneNumber: "更换手机号",
    clearDialogTitle: "清除账号授权？",
    clearDialogConsequence: "清除授权后，将移除所有摄像头信息。",
    cancel: "暂不清除",
    confirmClear: "清除授权",
    invalidCallingCode: "请输入有效的国家或地区区号，例如 +86",
    invalidNationalNumber: "米家手机号只能包含数字",
    phoneNumberTooLong: "国家区号与手机号合计不能超过 15 位数字",
    invalidSmsCode: "请输入 {length} 位短信验证码",
    smsSent: "短信验证码已发送",
    smsResent: "短信验证码已重新发送",
    authorizationCanceled: "手机号授权已取消",
    authorizationInterrupted: "授权会话已结束，请重新获取短信验证码",
    authorizationSucceeded: "小米账号授权成功",
    authorizationCleared: "授权已清除，请重新获取短信验证码。",
    reauthorizationRequired: "小米账号授权已失效，请先清除授权，再重新通过短信验证码授权。",
    regionChanged: "米家服务区已切换为 {region}",
    cameraStarted: "摄像头视频已切换",
    waitingInputNotice: "已保存摄像头，正在等待主视频输入切换到码流模式",
    busy: "当前操作仍在进行，请稍候",
    invalidRequest: "请求字段无效，请检查后重试",
    networkError: "网络连接失败，请稍后重试",
    providerError: "小米服务拒绝了请求，请稍后重试",
    protocolError: "服务响应无法识别，请稍后重试",
    requestFailed: "操作失败，请稍后重试",
    phone_account_not_found: "该手机号未关联小米账号",
    sms_code_invalid_or_expired: "短信验证码无效或已过期",
    additional_verification_required: "小米账号需要额外验证，暂时无法完成授权",
    xiaomi_token_response_invalid: "小米授权响应无法识别，请稍后重试",
    xiaomi_reauthorization_required: "小米账号授权已失效，请先清除授权，再重新通过短信验证码授权。",
    invalid_region: "米家服务区无效，请重新选择",
    catalog_incomplete: "摄像头目录暂时不完整，插件正在自动重试",
    catalog_retry_exhausted: "摄像头目录仍不完整，插件已自动重试 3 次",
    selected_camera_unavailable: "已保存的摄像头在当前地区不可用",
    sms_send_limit_tomorrow: "验证码发送过多，请明天再试",
    statusUnavailable: "暂时无法读取小米摄像头状态"
  },
  en: {
    skip: "Skip to main content",
    title: "Mi Camera",
    subtitle: "Connect your Mi Camera for AI analysis and RTSP forwarding.",
    rtspPanel: "RTSP forwarding",
    rtspToggle: "Enable RTSP forwarding",
    rtspAudio: "Forward audio",
    rtspAudioOn: "Disable audio",
    rtspAudioOff: "Enable audio",
    rtsp_audio_unsupported: "This camera does not support audio control",
    rtsp_reconnect_failed: "Could not update the camera connection. Check the device status.",
    rtsp_source_failed: "Camera connection failed. Check the camera and network.",
    rtspLoading: "Reading status…",
    rtspSaved: "RTSP setting saved",
    rtspAudioEnabled: "Audio enabled",
    rtspAudioDisabled: "Audio disabled",
    rtspReconnectRequested: "{setting}. The camera is reconnecting; existing RTSP connections need to reconnect.",
    rtspReadFailed: "Cannot read RTSP status. Check the device connection.",
    rtspWriteFailed: "The change could not be completed. Retry; the last confirmed setting is shown below.",
    rtspRuntimeFailed: "RTSP could not start. Check port availability or device status.",
    rtsp_control_failed: "RTSP could not confirm the setting. Check device status or port availability.",
    rtsp_persist_failed: "Could not save RTSP settings. Previous settings are unchanged; video may have stopped. Check the device status.",
    rtsp_persist_not_durable: "The RTSP setting was applied, but storage durability is unconfirmed. It may not survive a restart; retry saving.",
    rtsp_rollback_failed: "The RTSP change failed and rollback could not be confirmed. Service status is unknown.",
    rtsp_random_failed: "A new password could not be generated. The previous password is unchanged; try again later.",
    rtspAddress: "Stream URL",
    rtspNoAddress: "Waiting for a local device IP",
    rtspCopy: "Copy",
    rtspCopied: "Stream URL copied",
    rtspCopyFailed: "Copy failed. Select the text and copy it manually.",
    rtspSingleClient: "Note: 1 connection at a time. Disconnect the existing connection before establishing a new one, or click the refresh icon to generate a new URL",
    rtspRefreshAddress: "Refresh stream URL",
    rtspRotated: "Password refreshed. Copy the new Stream URL.",
    rtspRotateFailed: "Refresh unconfirmed. Copy the current Stream URL to reconnect.",
    rtspCredentialsUnknown: "Could not read the Stream URL.",
    rtspRetry: "Retry reading",
    authStatus: "Account",
    aiStatus: "AI",
    idle: "Unauthorized",
    working: "Working",
    sms_required: "SMS code required",
    authenticated: "Authorized",
    reauth_required: "Reauthorization required",
    stopped: "Stopped",
    starting: "Starting",
    running: "Running",
    waiting_input: "Waiting for bitstream",
    error: "Error",
    authPanel: "Account authorization",
    callingCode: "Country or region",
    nationalNumber: "Local phone number",
    getSmsCode: "Get SMS code",
    sendingSms: "Sending…",
    authorizedAccount: "Authorized account",
    accountRecovering: "Restoring saved account",
    authRetrying: "Waiting to retry",
    clearAuthorization: "Clear authorization",
    clearingAuthorization: "Clearing…",
    cameraPanel: "Camera",
    catalogRetrying: "Retrying the camera catalog automatically",
    catalogNetworkRetrying: "Xiaomi service unavailable. Retrying automatically.",
    catalogNetworkError: "Cannot reach Xiaomi services to load the camera catalog.",
    cameraRegion: "Device region",
    region_cn: "Mainland China",
    region_de: "Europe",
    region_i2: "India",
    region_ru: "Russia",
    region_sg: "Singapore & Southeast Asia",
    region_us: "United States",
    camera: "Camera",
    unassignedRoom: "Unassigned room",
    channelSelection: "Camera view",
    channelOne: "View 1",
    channelTwo: "View 2",
    smsCode: "SMS code",
    verifySms: "Verify and authorize",
    verifyingSms: "Verifying…",
    resendSms: "Resend",
    resendingSms: "Resending…",
    resendCountdown: "Resend in {seconds}s",
    changePhoneNumber: "Use another phone number",
    clearDialogTitle: "Clear account authorization?",
    clearDialogConsequence: "Clearing authorization will remove all camera information.",
    cancel: "Not now",
    confirmClear: "Clear authorization",
    invalidCallingCode: "Enter a valid calling code, such as +86",
    invalidNationalNumber: "The local phone number may contain digits only",
    phoneNumberTooLong: "The calling code and phone number may contain at most 15 digits",
    invalidSmsCode: "Enter the {length}-digit SMS code",
    smsSent: "SMS code sent",
    smsResent: "SMS code resent",
    authorizationCanceled: "Phone authorization canceled",
    authorizationInterrupted: "The authorization session ended. Request a new SMS code.",
    authorizationSucceeded: "Xiaomi account authorized",
    authorizationCleared: "Authorization cleared. Request a new SMS code.",
    reauthorizationRequired: "Xiaomi account authorization has expired. Clear the authorization, then verify again by SMS.",
    regionChanged: "Device region switched to {region}",
    cameraStarted: "Camera video switched",
    waitingInputNotice: "Camera saved; waiting for the main video input to switch to bitstream mode",
    busy: "An operation is already in progress",
    invalidRequest: "The request is invalid. Check the fields and retry.",
    networkError: "The network request failed. Try again later.",
    providerError: "The Xiaomi service rejected the request. Try again later.",
    protocolError: "The service response was not recognized. Try again later.",
    requestFailed: "The operation failed. Try again later.",
    phone_account_not_found: "No Xiaomi account is linked to this phone number.",
    sms_code_invalid_or_expired: "The SMS code is invalid or expired.",
    additional_verification_required: "This Xiaomi account needs an additional verification step that is not available here.",
    xiaomi_token_response_invalid: "The Xiaomi authorization response was not recognized. Try again later.",
    xiaomi_reauthorization_required: "Xiaomi account authorization has expired. Clear the authorization, then verify again by SMS.",
    invalid_region: "The device region is invalid. Select it again.",
    catalog_incomplete: "The camera catalog is temporarily incomplete. The plugin is retrying automatically.",
    catalog_retry_exhausted: "The camera catalog is still incomplete after 3 automatic retries.",
    selected_camera_unavailable: "The saved camera is unavailable in this region.",
    sms_send_limit_tomorrow: "Too many verification codes have been sent. Please try again tomorrow.",
    statusUnavailable: "Xiaomi camera status is temporarily unavailable"
  }
};

let language = Locale.get().language;
const AUTH_STATE_KEYS = Object.freeze({
  [AuthState.IDLE]: "idle",
  [AuthState.WORKING]: "working",
  [AuthState.SMS_REQUIRED]: "sms_required",
  [AuthState.AUTHENTICATED]: "authenticated",
  [AuthState.REAUTH_REQUIRED]: "reauth_required",
  [AuthState.ERROR]: "error",
  [AuthState.RETRYING]: "authRetrying"
});
const MEDIA_STATE_KEYS = Object.freeze({
  [MediaState.STOPPED]: "stopped",
  [MediaState.STARTING]: "starting",
  [MediaState.RUNNING]: "running",
  [MediaState.WAITING_INPUT]: "waiting_input",
  [MediaState.ERROR]: "error"
});
const CATALOG_ACTIVE_STATES = new Set([CatalogState.QUEUED, CatalogState.LOADING]);

let auth = { state_code: AuthState.IDLE, reason_code: 0, account: null, challenge: null, last_error: null };
let cameraStatus = { state_code: MediaState.STOPPED, selected: null, codec: null, last_error: null };
let cameras = [];
let cameraListState = CatalogState.IDLE;
let cameraListError = null;
let cameraRegion = "cn";
let pendingCameraRegion = null;
let cameraRenderKey = "";
let renderedCameraRegion = "";
let regionRenderKey = "";
let authAttemptActive = false;
let pendingAuthAction = "";
let pendingCancel = false;
let catalogPromise = null;
let catalogLoaded = false;
let pollTimer = 0;
let pollInFlight = false;
let initialStatusLoaded = false;
let pendingMediaAction = "";
let selectionTimer = 0;
let selectionInFlight = null;
let queuedSelection = null;
let regionInFlight = null;
let queuedRegion = null;
let pendingClear = false;
let channelChoiceTouched = false;
let clearDialog = null;
let reauthNoticeShown = false;
let countryPickerOpen = false;
let countryPickerRenderKey = "";
let rtsp = null;
let rtspLoadFailed = false;
let rtspWriteFailed = false;
let rtspRevision = 0;
let rtspCredentialsRevision = 0;
let rtspCredentials = null;
let rtspCredentialsNeedLoad = true;
let rtspCredentialError = "";
const gate = new MutationGate(createOperationId);
const notices = new NoticeDeduper();
const statusErrors = new StatusErrorTracker();

function t(key, replacements = {}) {
  let message = messages[language]?.[key] || messages.en[key] || key;
  for (const [name, value] of Object.entries(replacements)) message = message.replaceAll(`{${name}}`, String(value));
  return message;
}

function createOperationId() {
  if (typeof crypto.randomUUID === "function") return crypto.randomUUID();
  const bytes = new Uint8Array(16);
  crypto.getRandomValues(bytes);
  return Array.from(bytes, (value) => value.toString(16).padStart(2, "0")).join("");
}

function ref(name) { return app.querySelector(`[data-ref="${name}"]`); }
function setText(name, value) {
  const element = ref(name);
  if (element) element.textContent = value == null || value === "" ? "-" : String(value);
}
function setPane(name, visible) {
  const pane = app.querySelector(`[data-pane="${name}"]`);
  if (pane) pane.hidden = !visible;
}

function announce(message, urgent = false) {
  const element = ref(urgent ? "alert" : "announcer");
  if (!element) return;
  element.textContent = "";
  window.requestAnimationFrame(() => { element.textContent = message; });
}

function feedback(kind, key, scope = "", signature = "", replacements = {}) {
  if (scope && !notices.accept(scope, signature || key)) return false;
  const message = t(key, replacements);
  Toast[kind](message);
  announce(message, kind === "error");
  return true;
}

function showError(error, scope = "") { return feedback("error", errorMessageKey(error), scope, errorSignature(error)); }
function localizedState(keys, stateCode) { return t(keys[stateCode]); }
function focusInvalid(input) {
  if (!input) return;
  input.setAttribute("aria-invalid", "true");
  input.focus();
}
function clearInvalid(input) { input?.removeAttribute("aria-invalid"); }

function mount() {
  app.innerHTML = `
    <header class="topbar">
      <div class="title-block"><h1 data-l10n="title"></h1><p class="subtitle" data-l10n="subtitle"></p></div>
      <div class="status-badges" aria-live="polite" aria-atomic="true">
        <span class="status-badge" data-ref="auth-badge"><span data-l10n="authStatus"></span><strong data-ref="auth-state">-</strong></span>
        <span class="status-badge" data-ref="media-badge"><span data-l10n="aiStatus"></span><strong data-ref="media-state">-</strong></span>
      </div>
    </header>
    <section id="main-content" class="workspace" tabindex="-1">
      <div class="control-column">
      <article class="panel auth-panel" data-ref="auth-panel" aria-labelledby="auth-panel-title">
        <header class="panel-heading"><h2 id="auth-panel-title" data-l10n="authPanel"></h2></header>
        <div data-pane="unauthorized">
          <form class="form-stack" data-form="phone" autocomplete="off" novalidate>
            <div class="phone-fields">
              <div class="field"><span id="calling-code-label" data-l10n="callingCode"></span><div class="country-picker" data-ref="country-picker"><input data-ref="calling-code" type="hidden" value="${DEFAULT_COUNTRY_KEY}"><button id="calling-code" class="country-picker-trigger" data-ref="calling-code-trigger" type="button" aria-haspopup="listbox" aria-expanded="false" aria-controls="calling-code-listbox" aria-labelledby="calling-code-label calling-code-value" aria-required="true"><span id="calling-code-value" data-ref="calling-code-value"></span><span class="country-picker-chevron" aria-hidden="true"></span></button><div id="calling-code-listbox" class="country-picker-menu" data-ref="calling-code-listbox" role="listbox" aria-labelledby="calling-code-label" hidden></div></div></div>
              <label class="field" for="national-number"><span data-l10n="nationalNumber"></span><input id="national-number" data-ref="national-number" type="tel" inputmode="numeric" autocomplete="off" required></label>
            </div>
            <div class="panel-actions end"><button class="button primary" type="submit" data-action="sms-start" data-l10n="getSmsCode"></button></div>
          </form>
          <div class="inline-sms" data-pane="sms" hidden>
            <form class="challenge-form" data-form="sms" novalidate>
              <label class="field" for="sms-code"><span data-l10n="smsCode"></span><input id="sms-code" data-ref="sms-code" type="text" inputmode="numeric" autocomplete="one-time-code" spellcheck="false" required></label>
              <div class="sms-actions"><button class="button secondary" type="button" data-action="auth-cancel" data-l10n="changePhoneNumber"></button><button class="button primary" type="submit" data-action="sms-verify" data-l10n="verifySms"></button></div>
            </form>
          </div>
        </div>
        <div class="authorized-summary" data-pane="authorized" hidden>
          <div class="account-identity"><span class="identity-icon" aria-hidden="true">MI</span><div><span data-ref="account-caption"></span><strong data-ref="account-label">-</strong></div></div>
          <button class="button secondary danger" type="button" data-action="clear" data-l10n="clearAuthorization"></button>
        </div>
      </article>
      <article class="panel rtsp-panel" data-ref="rtsp-panel" aria-labelledby="rtsp-panel-title">
        <header class="panel-heading panel-heading-switch">
          <h2 id="rtsp-panel-title" data-l10n="rtspPanel"></h2>
          <button class="rtsp-switch" data-ref="rtsp-toggle" type="button" role="switch" aria-checked="false" aria-labelledby="rtsp-toggle-label" disabled><span aria-hidden="true"></span></button><span class="visually-hidden" id="rtsp-toggle-label" data-l10n="rtspToggle"></span>
        </header>
        <div class="rtsp-address-heading"><span id="rtsp-address-label" data-l10n="rtspAddress"></span>
          <div class="rtsp-address-actions">
            <button class="rtsp-icon-button rtsp-audio" data-ref="rtsp-audio" type="button" role="switch" aria-checked="false" disabled><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false"><path d="M11 4 6 8H3v8h3l5 4Z"/><path class="rtsp-audio-waves" d="M15 8a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14"/><path class="rtsp-audio-slash" d="m3 3 18 18"/></svg></button>
            <button class="rtsp-icon-button" data-ref="rtsp-rotate" type="button" disabled><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true" focusable="false"><path d="M20 7v5h-5M4 17v-5h5M6.1 6.1A8 8 0 0 1 19.8 11M4.2 13A8 8 0 0 0 17.9 17.9"/></svg></button>
          </div>
        </div>
        <p class="rtsp-error" data-ref="rtsp-error" role="alert" hidden></p>
        <div class="rtsp-address-box">
          <code data-ref="rtsp-url" class="rtsp-url" tabindex="0" aria-labelledby="rtsp-address-label"></code>
          <button class="rtsp-copy" data-ref="rtsp-copy" type="button" data-l10n="rtspCopy" disabled></button>
        </div>
        <p class="rtsp-error" data-ref="rtsp-credential-error" role="alert" hidden></p>
        <button class="rtsp-retry" data-ref="rtsp-retry" type="button" data-l10n="rtspRetry" hidden></button>
        <p class="rtsp-client-limit" data-l10n="rtspSingleClient"></p>
      </article>
      </div>
      <article class="panel camera-panel" data-ref="camera-panel" aria-labelledby="camera-panel-title">
        <header class="panel-heading"><h2 id="camera-panel-title" data-l10n="cameraPanel"></h2></header>
        <div class="camera-controls">
          <p class="subtitle" data-ref="catalog-recovery" role="status" hidden></p>
          <label class="field" for="camera-region"><span data-l10n="cameraRegion"></span><select id="camera-region" data-ref="camera-region"></select></label>
          <label class="field" for="camera-select"><span data-l10n="camera"></span><select id="camera-select" data-ref="camera-select"></select></label>
          <fieldset class="channel-field" data-ref="channel-field"><legend data-l10n="channelSelection"></legend><div class="channel-options"><label><input type="radio" name="camera-channel" value="1" checked><span data-l10n="channelOne"></span></label><label><input type="radio" name="camera-channel" value="2"><span data-l10n="channelTwo"></span></label></div></fieldset>
        </div>
      </article>
    </section>
    <dialog class="modal clear-dialog" data-ref="clear-dialog" role="alertdialog" aria-modal="true" aria-labelledby="clear-dialog-title" aria-describedby="clear-dialog-consequence"><div class="modal-surface"><header class="modal-heading"><h2 id="clear-dialog-title" data-l10n="clearDialogTitle"></h2><p id="clear-dialog-consequence" data-l10n="clearDialogConsequence"></p></header><footer class="modal-actions"><button class="button secondary" type="button" data-action="clear-cancel" data-l10n="cancel"></button><button class="button danger" type="button" data-action="clear-confirm" data-l10n="confirmClear"></button></footer></div></dialog>
    <div class="visually-hidden" data-ref="announcer" aria-live="polite" aria-atomic="true"></div><div class="visually-hidden" data-ref="alert" role="alert" aria-live="assertive" aria-atomic="true"></div>
  `;
  bindEvents();
  applyLocale();
}

function renderCallingCodeOptions() {
  const input = ref("calling-code");
  const trigger = ref("calling-code-trigger");
  const listbox = ref("calling-code-listbox");
  if (!input || !trigger || !listbox) return;
  const selectedKey = callingCodeForCountryKey(input.value) ? input.value : DEFAULT_COUNTRY_KEY;
  const selectedCountry = COUNTRY_CALLING_CODES.find((country) => countryKey(country) === selectedKey);
  input.value = selectedKey;
  ref("calling-code-value").textContent = countryOptionLabel(selectedCountry);
  if (countryPickerRenderKey !== language) {
    countryPickerRenderKey = language;
    listbox.replaceChildren();
    for (const country of COUNTRY_CALLING_CODES) {
      const option = document.createElement("button");
      const name = document.createElement("span");
      const code = document.createElement("span");
      option.type = "button";
      option.className = "country-picker-option";
      option.dataset.countryKey = countryKey(country);
      option.setAttribute("role", "option");
      option.tabIndex = -1;
      name.className = "country-picker-name";
      name.textContent = language === "zh" ? country.name_zh : country.name_en;
      code.className = "country-picker-code";
      code.textContent = country.calling_code;
      option.append(name, code);
      listbox.append(option);
    }
  }
  for (const option of countryPickerOptions()) {
    option.setAttribute("aria-selected", String(option.dataset.countryKey === selectedKey));
  }
  trigger.setAttribute("aria-expanded", String(countryPickerOpen));
  listbox.hidden = !countryPickerOpen;
}

function countryOptionLabel(country) {
  if (!country) return "-";
  return `${language === "zh" ? country.name_zh : country.name_en} (${country.calling_code})`;
}

function countryPickerOptions() {
  return Array.from(ref("calling-code-listbox")?.querySelectorAll("[role='option']") || []);
}

function focusCountryOption(position = "selected") {
  const options = countryPickerOptions();
  if (!options.length) return;
  let option = options.find((item) => item.dataset.countryKey === ref("calling-code")?.value);
  if (position === "first") option = options[0];
  else if (position === "last") option = options[options.length - 1];
  (option || options[0]).focus({ preventScroll: true });
  (option || options[0]).scrollIntoView({ block: "nearest" });
}

function setCountryPickerOpen(open, focusPosition = "") {
  const trigger = ref("calling-code-trigger");
  if (!trigger || (open && trigger.disabled)) return;
  countryPickerOpen = Boolean(open);
  renderCallingCodeOptions();
  if (countryPickerOpen && focusPosition) {
    queueMicrotask(() => focusCountryOption(focusPosition));
  } else if (!countryPickerOpen && focusPosition === "trigger") {
    trigger.focus();
  }
}

function selectCountry(key) {
  if (!callingCodeForCountryKey(key)) return;
  ref("calling-code").value = key;
  clearInvalid(ref("calling-code-trigger"));
  countryPickerOpen = false;
  renderCallingCodeOptions();
  ref("calling-code-trigger").focus();
}

function moveCountryOption(event, delta) {
  const options = countryPickerOptions();
  if (!options.length) return;
  const current = event.target.closest("[role='option']");
  const index = Math.max(0, options.indexOf(current));
  const next = options[(index + delta + options.length) % options.length];
  next.focus({ preventScroll: true });
  next.scrollIntoView({ block: "nearest" });
}

function handleCountryTriggerKeydown(event) {
  switch (event.key) {
  case "ArrowDown":
    event.preventDefault();
    setCountryPickerOpen(true, "selected");
    break;
  case "ArrowUp":
    event.preventDefault();
    setCountryPickerOpen(true, "selected");
    break;
  case "Home":
    event.preventDefault();
    setCountryPickerOpen(true, "first");
    break;
  case "End":
    event.preventDefault();
    setCountryPickerOpen(true, "last");
    break;
  case "Escape":
    if (countryPickerOpen) event.preventDefault();
    setCountryPickerOpen(false);
    break;
  case "Enter":
  case " ":
    event.preventDefault();
    setCountryPickerOpen(!countryPickerOpen, countryPickerOpen ? "" : "selected");
    break;
  }
}

function handleCountryListboxKeydown(event) {
  switch (event.key) {
  case "ArrowDown":
    event.preventDefault();
    moveCountryOption(event, 1);
    break;
  case "ArrowUp":
    event.preventDefault();
    moveCountryOption(event, -1);
    break;
  case "Home":
    event.preventDefault();
    focusCountryOption("first");
    break;
  case "End":
    event.preventDefault();
    focusCountryOption("last");
    break;
  case "Escape":
    event.preventDefault();
    setCountryPickerOpen(false, "trigger");
    break;
  case "Enter":
  case " ": {
    const option = event.target.closest("[role='option']");
    if (!option) break;
    event.preventDefault();
    selectCountry(option.dataset.countryKey);
    break;
  }
  case "Tab":
    event.preventDefault();
    setCountryPickerOpen(false);
    (event.shiftKey ? ref("calling-code-trigger") : ref("national-number")).focus();
    break;
  }
}

function applyLocale() {
  document.title = t("title");
  document.querySelector(".skip-link").textContent = t("skip");
  app.querySelectorAll("[data-l10n]").forEach((element) => { element.textContent = t(element.dataset.l10n); });
  app.querySelectorAll("[data-l10n-placeholder]").forEach((element) => { element.placeholder = t(element.dataset.l10nPlaceholder); });
  renderCallingCodeOptions();
  render();
}

function accountLabel() { return typeof auth?.account?.masked_id === "string" && auth.account.masked_id ? auth.account.masked_id : "-"; }
function renderBadges() {
  const visibleAuthState = authDisplayState(auth);
  const authKey = AUTH_STATE_KEYS[visibleAuthState];
  const mediaKey = MEDIA_STATE_KEYS[cameraStatus.state_code];
  setText("auth-state", localizedState(AUTH_STATE_KEYS, visibleAuthState));
  setText("media-state", localizedState(MEDIA_STATE_KEYS, cameraStatus.state_code));
  ref("auth-badge").dataset.state = authKey;
  ref("media-badge").dataset.state = mediaKey;
}
function authorizationMutationBusy() {
  return ["auth-sms-start", "auth-sms-verify", "auth-sms-resend"].some((key) => gate.has(key));
}
function renderAuthorization() {
  const hasAccount = Boolean(auth.account);
  const recovering = auth.state_code === AuthState.RETRYING;
  const smsChallenge = auth.state_code === AuthState.SMS_REQUIRED && auth.challenge?.kind === "sms";
  if (hasAccount && countryPickerOpen) setCountryPickerOpen(false);
  setPane("unauthorized", !hasAccount && !recovering);
  setPane("authorized", hasAccount || recovering);
  setText("account-caption", t(recovering ? "accountRecovering" : "authorizedAccount"));
  setPane("sms", !hasAccount && smsChallenge);
  setText("account-label", accountLabel());
  ref("auth-panel").dataset.state = AUTH_STATE_KEYS[auth.state_code];
  ref("auth-panel").setAttribute("aria-busy", String(authorizationMutationBusy() || gate.has("auth-clear")));
}
function selectedRegion() {
  return queuedRegion || regionInFlight || pendingCameraRegion || cameraRegion || "cn";
}
function regionLabel(region) { return t(`region_${region}`); }
function renderRegionSelect() {
  const select = ref("camera-region");
  if (!select) return;
  const nextKey = JSON.stringify([language, CAMERA_REGIONS]);
  if (nextKey !== regionRenderKey) {
    regionRenderKey = nextKey;
    select.replaceChildren();
    for (const region of CAMERA_REGIONS) {
      const option = document.createElement("option");
      option.value = region;
      option.textContent = regionLabel(region);
      select.append(option);
    }
  }
  select.value = selectedRegion();
}
function visibleCameras() {
  return selectedRegion() === cameraRegion ? cameras : [];
}
function renderCameraSelect() {
  const select = ref("camera-select");
  if (!select) return;
  const items = visibleCameras();
  const displayedRegion = selectedRegion();
  const selected =
    (queuedSelection?.region === displayedRegion ? queuedSelection.cameraId : "") ||
    (selectionInFlight?.region === displayedRegion ? selectionInFlight.cameraId : "") ||
    (cameraStatus?.selected?.region === displayedRegion ? cameraStatus.selected.id : "") ||
    (renderedCameraRegion === displayedRegion ? select.value : "");
  const nextKey = JSON.stringify([
    language,
    cameraListState,
    selectedRegion(),
    selected,
    items.map((item) => [item.id, item.name, item.model, item.home_id, item.home_name, item.room_id, item.room_name])
  ]);
  if (nextKey === cameraRenderKey) return;
  cameraRenderKey = nextKey;
  renderedCameraRegion = displayedRegion;
  select.replaceChildren();
  const groups = new Map();
  for (const camera of items) {
    const key = JSON.stringify([camera.home_id, camera.home_name, camera.room_id, camera.room_name]);
    let group = groups.get(key);
    if (!group) {
      const room = camera.room_name || t("unassignedRoom");
      const label = [camera.home_name, room].filter(Boolean).join(" · ");
      group = document.createElement("optgroup");
      group.label = label;
      groups.set(key, group);
      select.append(group);
    }
    const option = document.createElement("option");
    option.value = camera.id;
    option.textContent = camera.name || camera.id;
    group.append(option);
  }
  const hasSelected = items.some((camera) => camera.id === selected);
  if (hasSelected) select.value = selected;
  else select.selectedIndex = -1;
}
function selectedChannel() { return Number(app.querySelector('input[name="camera-channel"]:checked')?.value || 1); }
function renderCamera() {
  renderRegionSelect();
  renderCameraSelect();
  const recovering = auth.state_code === AuthState.AUTHENTICATED && cameraListState === CatalogState.RETRYING;
  ref("catalog-recovery").hidden = !recovering;
  setText("catalog-recovery", recovering ? t(cameraListError?.category === "network" ? "catalogNetworkRetrying" : "catalogRetrying") : "");
  const applying = Boolean(selectionTimer || selectionInFlight || queuedSelection || regionInFlight || queuedRegion);
  ref("camera-panel").setAttribute("aria-busy", String(Boolean(catalogPromise) || applying));
}
function setButtonState(action, busy, busyKey) {
  const button = app.querySelector(`[data-action="${action}"]`);
  if (!button) return;
  button.textContent = t(busy ? busyKey : button.dataset.l10n);
  button.setAttribute("aria-busy", String(busy));
}

function renderBusy() {
  const authorized = auth.state_code === AuthState.AUTHENTICATED && Boolean(auth.account);
  const authBusy = authorizationMutationBusy();
  const clearBusy = gate.has("auth-clear") || pendingClear;
  const cancelBusy = gate.has("auth-cancel") || pendingCancel;
  const listBusy = Boolean(catalogPromise) || CATALOG_ACTIVE_STATES.has(cameraListState);
  const regionBusy = Boolean(regionInFlight || queuedRegion || pendingCameraRegion);
  const anyAuthMutation = authBusy || clearBusy || cancelBusy;
  app.setAttribute("aria-busy", String(gate.busy || listBusy || pollInFlight));
  setButtonState("sms-verify", gate.has("auth-sms-verify"), "verifyingSms");
  setButtonState("clear", clearBusy, "clearingAuthorization");
  const smsChallenge = auth.state_code === AuthState.SMS_REQUIRED && auth.challenge?.kind === "sms";
  const retryAfter = smsChallenge ? auth.challenge.retry_after_seconds : 0;
  const authStateWorking = [AuthState.WORKING, AuthState.RETRYING].includes(auth.state_code);
  const startBusy = gate.has("auth-sms-start");
  const resendBusy = gate.has("auth-sms-resend");
  const startAuthorization = app.querySelector("[data-action='sms-start']");
  if (startAuthorization) {
    startAuthorization.textContent = startBusy
      ? t("sendingSms")
      : resendBusy
        ? t("resendingSms")
        : smsChallenge
          ? retryAfter > 0 ? t("resendCountdown", { seconds: retryAfter }) : t("resendSms")
          : t("getSmsCode");
    startAuthorization.disabled = authStateWorking || authBusy || clearBusy || cancelBusy || (smsChallenge && retryAfter > 0);
    startAuthorization.setAttribute("aria-busy", String(startBusy || resendBusy));
  }
  const phoneLocked = authInteractionPolicy(auth.state_code, authAttemptActive).phoneLocked;
  ref("calling-code-trigger").disabled = authBusy || clearBusy || phoneLocked;
  ref("national-number").disabled = authBusy || clearBusy || phoneLocked;
  const verify = app.querySelector("[data-action='sms-verify']");
  if (verify) verify.disabled = authBusy || cancelBusy;
  const clear = app.querySelector("[data-action='clear']");
  if (clear) clear.disabled = anyAuthMutation || listBusy || regionBusy || gate.has("camera-start");
  ref("camera-region").disabled = !authorized || anyAuthMutation || clearBusy;
  ref("camera-select").disabled = !authorized || anyAuthMutation || listBusy || regionBusy || !visibleCameras().length;
  ref("channel-field").disabled = !authorized || anyAuthMutation || listBusy || regionBusy || !ref("camera-select").value;
  const cancelAuthorization = app.querySelector("[data-action='auth-cancel']");
  if (cancelAuthorization) cancelAuthorization.disabled = authBusy || cancelBusy;
}

function renderSmsChallenge() {
  if (auth.state_code === AuthState.SMS_REQUIRED && auth?.challenge?.kind === "sms") {
    const challenge = auth.challenge;
    const smsCode = ref("sms-code");
    smsCode.maxLength = challenge.code_length;
  }
}
function renderRtsp() {
  const rotating = gate.has("rtsp-rotate");
  const credentialsBusy = gate.has("rtsp-credentials") || rotating;
  const busy = gate.has("rtsp-setting") || rotating;
  const audioSupported = rtsp?.audio_supported === true;
  ref("rtsp-toggle").setAttribute("aria-checked", String(rtsp?.enabled === true));
  ref("rtsp-toggle").disabled = !rtsp || rtspLoadFailed || busy;
  ref("rtsp-audio").setAttribute("aria-checked", String(rtsp?.audio_enabled === true));
  ref("rtsp-audio").dataset.supported = String(audioSupported);
  ref("rtsp-audio").setAttribute("aria-label", t(audioSupported ? "rtspAudio" : "rtsp_audio_unsupported"));
  ref("rtsp-audio").title = t(!audioSupported ? "rtsp_audio_unsupported" : rtsp.audio_enabled ? "rtspAudioOn" : "rtspAudioOff");
  ref("rtsp-audio").disabled = !audioSupported || rtspLoadFailed || busy;
  ref("rtsp-panel").setAttribute("aria-busy", String(busy));
  const runtimeErrorKey = rtsp?.error ? errorMessageKey(rtsp.error) : "";
  const errorKey = rtspLoadFailed ? "rtspReadFailed" : rtsp?.error ? (runtimeErrorKey === "requestFailed" ? "rtspRuntimeFailed" : runtimeErrorKey) : rtsp?.state === RTSPState.ERROR ? "rtspRuntimeFailed" : rtspWriteFailed ? "rtspWriteFailed" : "";
  ref("rtsp-error").hidden = !errorKey;
  setText("rtsp-error", errorKey ? t(errorKey) : "");
  const address = document.visibilityState === "hidden" ? "" : !rtsp?.host ? t("rtspNoAddress") : rtspCredentials ? rtspPlaybackAddress(rtspCredentials) : rtspCredentialError ? "—" : t("rtspLoading");
  if (ref("rtsp-url").textContent !== address) ref("rtsp-url").textContent = address;
  ref("rtsp-copy").disabled = !rtsp?.host || rtspLoadFailed || credentialsBusy;
  ref("rtsp-rotate").disabled = !rtsp || rtspLoadFailed || busy;
  ref("rtsp-rotate").setAttribute("aria-busy", String(rotating));
  ref("rtsp-rotate").setAttribute("aria-label", t("rtspRefreshAddress"));
  ref("rtsp-rotate").title = t("rtspRefreshAddress");
  ref("rtsp-credential-error").hidden = !rtspCredentialError || Boolean(errorKey);
  setText("rtsp-credential-error", rtspCredentialError ? t(rtspCredentialError) : "");
  ref("rtsp-retry").hidden = !rtspCredentialError;
  ref("rtsp-retry").disabled = credentialsBusy;
}
function render() { renderBadges(); renderAuthorization(); renderCamera(); renderSmsChallenge(); renderBusy(); renderRtsp(); }

async function refreshRtsp() {
  // Never let a pre-mutation poll overwrite the backend-confirmed mutation result.
  if (gate.has("rtsp-setting") || gate.has("rtsp-rotate")) return;
  const revision = rtspRevision;
  try {
    const result = await api.rtspStatus();
    if (revision === rtspRevision) { rtsp = result; rtspLoadFailed = false; }
  } catch {
    if (revision === rtspRevision) rtspLoadFailed = true;
  }
  renderRtsp();
}
function changeRtspSetting(field) {
  if (!rtsp || rtspLoadFailed || gate.has("rtsp-setting") || gate.has("rtsp-rotate")) return;
  if (field === "audio_enabled" && !rtsp.audio_supported) return;
  const desired = !rtsp[field];
  rtspRevision++;
  rtspWriteFailed = false;
  void gate.run("rtsp-setting", async () => {
    try {
      rtsp = await (field === "enabled" ? api.setRtspEnabled(desired) : api.setRtspAudioEnabled(desired));
      const savedKey = field === "audio_enabled"
        ? (rtsp.audio_enabled ? "rtspAudioEnabled" : "rtspAudioDisabled")
        : "rtspSaved";
      feedback("success", rtsp.reconnect_requested ? "rtspReconnectRequested" : savedKey,
        "", "", { setting: t(savedKey) });
    } catch (error) {
      rtspWriteFailed = true;
      showError(error);
      try { rtsp = await api.rtspStatus(); rtspLoadFailed = false; }
      catch { rtspLoadFailed = true; }
    }
  }).finally(() => { renderRtsp(); schedulePoll(120); });
  renderRtsp();
}
async function copyRtspText(value, successKey, input) {
  try {
    if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(value);
    else {
      // Product LAN pages use HTTP; Clipboard API requires a secure context.
      const selection = document.createElement("textarea");
      const focused = document.activeElement;
      selection.value = value;
      selection.className = "clipboard-selection";
      document.body.append(selection);
      selection.select();
      let copied;
      try { copied = document.execCommand("copy"); } finally { selection.remove(); focused?.focus({ preventScroll: true }); }
      if (!copied) throw new Error("clipboard unavailable");
    }
    feedback("success", successKey);
    return true;
  } catch {
    input?.focus();
    if (input) {
      const range = document.createRange();
      range.selectNodeContents(input);
      window.getSelection()?.removeAllRanges();
      window.getSelection()?.addRange(range);
    }
    feedback("warning", "rtspCopyFailed");
    return false;
  }
}
function clearRtspCredentials() {
  rtspCredentialsRevision++;
  if (rtspCredentials) rtspCredentials.password = "";
  rtspCredentials = null;
  renderRtsp();
}
function rtspPlaybackAddress(credentials) {
  if (!rtsp?.host || !credentials) return "";
  const username = encodeURIComponent(credentials.username);
  const password = encodeURIComponent(credentials.password);
  return `rtsp://${username}:${password}@${rtsp.host}:${rtsp.port}/stream`;
}
function loadRtspCredentials(copy = false) {
  if (gate.has("rtsp-credentials") || gate.has("rtsp-rotate") || document.visibilityState === "hidden") return;
  rtspCredentialsNeedLoad = false;
  const revision = rtspCredentialsRevision;
  rtspCredentialError = "";
  void gate.run("rtsp-credentials", async () => {
    const credentials = await api.rtspCredentials();
    try {
      if (document.visibilityState === "hidden" || revision !== rtspCredentialsRevision) return;
      if (rtspCredentials) rtspCredentials.password = "";
      rtspCredentials = { ...credentials };
      renderRtsp();
      if (copy && rtsp?.host) await copyRtspText(rtspPlaybackAddress(credentials), "rtspCopied", ref("rtsp-url"));
    } finally { credentials.password = ""; }
  }).catch((error) => {
    if (revision === rtspCredentialsRevision) {
      clearRtspCredentials();
      rtspCredentialError = "rtspCredentialsUnknown";
      showError(error);
    }
  }).finally(() => {
    renderRtsp();
    if (rtspCredentialsNeedLoad) loadRtspCredentials();
  });
  renderRtsp();
}
function rotateRtspPassword() {
  if (gate.has("rtsp-rotate") || gate.has("rtsp-setting")) return;
  clearRtspCredentials();
  rtspCredentialsNeedLoad = false;
  const revision = rtspCredentialsRevision;
  rtspRevision++;
  rtspCredentialError = "";
  void gate.run("rtsp-rotate", async () => {
    let credentials = null;
    try {
      credentials = await api.rotateRtspCredentials();
      feedback("success", "rtspRotated");
    } catch (error) {
      rtspCredentialError = "rtspRotateFailed";
      showError(error);
      // A response failure may follow a committed rotation. Only read to reconcile.
      try { credentials = await api.rtspCredentials(); }
      catch { rtspCredentialError = "rtspCredentialsUnknown"; }
    }
    try {
      rtsp = await api.rtspStatus();
      rtspLoadFailed = false;
    } catch { rtspLoadFailed = true; }
    if (credentials) {
      if (revision === rtspCredentialsRevision && document.visibilityState !== "hidden") {
        rtspCredentials = { ...credentials };
      }
      credentials.password = "";
    }
  }).finally(() => {
    renderRtsp();
    if (rtspCredentialsNeedLoad) loadRtspCredentials();
    schedulePoll(120);
  });
  renderRtsp();
}

function clearAuthorizationSecrets() {
  consumeSecret(ref("national-number"), true);
  consumeSecret(ref("sms-code"), true);
  auth = { ...auth, challenge: null };
  authAttemptActive = false;
  clearInvalid(ref("calling-code-trigger"));
  for (const name of ["national-number", "sms-code"]) clearInvalid(ref(name));
  renderCallingCodeOptions();
}
function resetChannelChoice() {
  const mainView = app.querySelector('input[name="camera-channel"][value="1"]');
  if (mainView) mainView.checked = true;
  channelChoiceTouched = false;
}
function focusAuthChallenge() {
  queueMicrotask(() => {
    if (auth.state_code === AuthState.SMS_REQUIRED) ref("sms-code")?.focus();
  });
}
function projectAuth(result) {
  auth = normalizeAuthResult(result);
  const notice = reauthNoticeTransition(auth.state_code, reauthNoticeShown);
  reauthNoticeShown = notice.shown;
  if (notice.show) {
    feedback("error", "reauthorizationRequired");
  }
}
function projectCameraStatus(result) {
  cameraStatus = normalizeCameraStatusResult(result);
  const channel = cameraStatus.selected?.channel;
  if (!channelChoiceTouched && !selectionInFlight && !queuedSelection && (channel === 1 || channel === 2)) {
    const input = app.querySelector(`input[name="camera-channel"][value="${channel}"]`);
    if (input) input.checked = true;
  }
}
function projectCameraList(result) {
  const projection = normalizeCameraListResult(result);
  cameraListState = projection.state_code;
  cameraListError = projection.last_error;
  cameraRegion = projection.region;
  pendingCameraRegion = projection.pending_region;
  cameras = projection.items;
  cameraRenderKey = "";
  return projection;
}
function settleAuthorizationFlow() {
  if (pendingCancel) return;
  if (auth.state_code === AuthState.SMS_REQUIRED) {
    if (pendingAuthAction === "resend") feedback("success", "smsResent", "auth-step", "resend");
    else if (pendingAuthAction === "start") {
      feedback("success", "smsSent", "auth-step", "start");
    }
    pendingAuthAction = "";
    authAttemptActive = true;
    render();
    focusAuthChallenge();
    return;
  }
  if (!authAttemptActive) return;
  if (auth.state_code === AuthState.AUTHENTICATED) {
    pendingAuthAction = "";
    feedback("success", "authorizationSucceeded", "auth-terminal", "authenticated");
    clearAuthorizationSecrets();
    render();
    queueMicrotask(() => app.querySelector("[data-action='clear']")?.focus());
    cameras = [];
    cameraListState = CatalogState.IDLE;
    cameraRenderKey = "";
    catalogLoaded = false;
  } else if (auth.state_code === AuthState.ERROR) {
    pendingAuthAction = "";
    showError(auth.last_error || { message_key: "request_failed" }, "auth-terminal");
    clearAuthorizationSecrets();
    render();
  } else if (auth.state_code === AuthState.IDLE) {
    pendingAuthAction = "";
    feedback("error", "authorizationInterrupted", "auth-terminal", "idle");
    clearAuthorizationSecrets();
    render();
  } else {
    render();
  }
}
function runAuthorizationMutation(key, task) {
  if (!authInteractionPolicy(auth.state_code, authAttemptActive).attemptActive) return Promise.resolve();
  render();
  const promise = gate.run(key, task).then(() => schedulePoll(120)).catch((error) => {
    pendingAuthAction = "";
    showError(error, "auth-terminal");
    if (key === "auth-sms-start") clearAuthorizationSecrets();
  }).finally(render);
  render();
  return promise;
}
function cancelAuthorizationAttempt() {
  pendingCancel = true;
  pendingAuthAction = "";
  const promise = gate.run("auth-cancel", (operationId) => api.cancelAuthorization(operationId))
    .then(() => schedulePoll(120))
    .catch((error) => {
      pendingCancel = false;
      showError(error, "auth-cancel");
    })
    .finally(render);
  render();
  return promise;
}

function loadCameraList(refresh) {
  if (catalogPromise) return catalogPromise;
  let tracked;
  const request = (async () => {
    let requestRefresh = refresh;
    while (true) {
      await waitForVisibleDocument(document);
      const result = await api.cameraList(requestRefresh);
      requestRefresh = false;
      const projection = projectCameraList(result);
      render();
      if (CATALOG_ACTIVE_STATES.has(projection.state_code)) {
        await new Promise((resolve) => window.setTimeout(resolve, 800));
        continue;
      }
      if (projection.state_code === CatalogState.RETRYING) {
        catalogLoaded = false;
        schedulePoll(800);
        return result;
      }
      if (projection.state_code === CatalogState.ERROR || projection.last_error) {
        const error = projection.last_error || { message_key: "request_failed" };
        if (error.category === "network") feedback("error", "catalogNetworkError", "camera-list-terminal", errorSignature(error));
        else showError(error, "camera-list-terminal");
        catalogLoaded = true;
        return result;
      }
      notices.clear("camera-list-terminal");
      catalogLoaded = projection.state_code === CatalogState.READY;
      return result;
    }
  })();
  tracked = request.catch((error) => {
    catalogLoaded = false;
    if (errorMessageKey(error) !== "busy") {
      showError(error, "camera-list-terminal");
    }
    return null;
  }).finally(() => {
    if (catalogPromise === tracked) {
      catalogPromise = null;
      runQueuedRegion();
    }
    render();
  });
  catalogPromise = tracked;
  render();
  return tracked;
}
function cancelQueuedSelection() {
  if (selectionTimer) window.clearTimeout(selectionTimer);
  selectionTimer = 0;
  queuedSelection = null;
  channelChoiceTouched = false;
}
function queueRegionFromControl() {
  if (auth.state_code !== AuthState.AUTHENTICATED) return;
  const region = ref("camera-region")?.value || "";
  if (!CAMERA_REGIONS.includes(region)) return;
  cancelQueuedSelection();
  cameraRenderKey = "";
  if (!regionInFlight && region === cameraRegion && pendingCameraRegion === null) {
    queuedRegion = null;
    render();
    return;
  }
  queuedRegion = region;
  runQueuedRegion();
  render();
}
function runQueuedRegion() {
  if (regionInFlight || !queuedRegion || catalogPromise || gate.has("camera-start") || selectionInFlight) return;
  const region = queuedRegion;
  queuedRegion = null;
  regionInFlight = region;
  catalogLoaded = false;
  cameraRenderKey = "";
  const promise = gate.run("camera-region", (operationId) => api.setCameraRegion(operationId, region))
    .then(() => {
      schedulePoll(120);
      return loadCameraList(false);
    })
    .then(() => {
      if (cameraListState === CatalogState.READY && cameraRegion === region && !queuedRegion) {
        feedback("success", "regionChanged", "camera-region", region, { region: regionLabel(region) });
      }
    })
    .catch((error) => {
      if (errorMessageKey(error) !== "busy") showError(error, "camera-region-error");
      else feedback("warning", "busy", "camera-region-error", errorSignature(error));
    })
    .finally(() => {
      regionInFlight = null;
      cameraRenderKey = "";
      if (queuedRegion && queuedRegion !== cameraRegion) runQueuedRegion();
      else if (queuedRegion === cameraRegion) queuedRegion = null;
      render();
    });
  render();
  return promise;
}
function sameSelection(left, right) {
  return Boolean(
    left && right &&
    left.region === right.region &&
    left.cameraId === right.cameraId &&
    left.channel === right.channel
  );
}
function statusSelection() {
  const selected = cameraStatus?.selected;
  return selected ? {
    region: selected.region,
    cameraId: selected.id,
    channel: selected.channel
  } : null;
}
function scheduleQueuedSelection(delay = 300) {
  if (!queuedSelection || gate.has("camera-start")) return;
  if (!selectionInFlight && sameSelection(queuedSelection, statusSelection()) &&
      [MediaState.RUNNING, MediaState.WAITING_INPUT].includes(cameraStatus.state_code)) {
    queuedSelection = null;
    channelChoiceTouched = false;
    render();
    return;
  }
  if (selectionTimer) window.clearTimeout(selectionTimer);
  selectionTimer = window.setTimeout(() => {
    selectionTimer = 0;
    void applyQueuedSelection();
  }, delay);
  render();
}
function queueSelectionFromControls() {
  if (
    auth.state_code !== AuthState.AUTHENTICATED ||
    regionInFlight || queuedRegion || pendingCameraRegion ||
    ![CatalogState.READY, CatalogState.ERROR].includes(cameraListState)
  ) return;
  const cameraId = ref("camera-select")?.value || "";
  if (!visibleCameras().some((camera) => camera.id === cameraId)) return;
  const activeSelection = statusSelection();
  const cameraChanged = !activeSelection ||
    activeSelection.region !== cameraRegion || activeSelection.cameraId !== cameraId;
  if (cameraChanged) resetChannelChoice();
  queuedSelection = {
    region: cameraRegion,
    cameraId,
    channel: cameraChanged ? 1 : selectedChannel()
  };
  scheduleQueuedSelection(300);
  render();
}
function applyQueuedSelection() {
  if (!queuedSelection || gate.has("camera-start")) return Promise.resolve();
  const selection = queuedSelection;
  const previousSelection = selectionInFlight;
  queuedSelection = null;
  selectionInFlight = selection;
  pendingMediaAction = "start";
  notices.clear("camera-action");
  const promise = gate.run("camera-start", (operationId) => (
    api.selectCamera(operationId, selection.region, selection.cameraId, selection.channel)
  )).then((result) => {
    schedulePoll(120);
    return result;
  }).catch((error) => {
    if (errorMessageKey(error) === "busy") {
      if (!queuedRegion && !queuedSelection) queuedSelection = selection;
      selectionInFlight = previousSelection;
      pendingMediaAction = previousSelection ? "start" : "";
      if (queuedSelection) scheduleQueuedSelection(300);
      else runQueuedRegion();
      return;
    }
    pendingMediaAction = "";
    selectionInFlight = null;
    showError(error, "camera-action-error");
    runQueuedRegion();
  }).finally(() => {
    if (queuedSelection && !queuedRegion) scheduleQueuedSelection(300);
    render();
  });
  render();
  return promise;
}
function continueQueuedSelection() {
  if (queuedRegion) {
    cancelQueuedSelection();
    runQueuedRegion();
    return;
  }
  if (!queuedSelection) return;
  if (!selectionInFlight && sameSelection(queuedSelection, statusSelection()) &&
      [MediaState.RUNNING, MediaState.WAITING_INPUT].includes(cameraStatus.state_code)) {
    queuedSelection = null;
    channelChoiceTouched = false;
    return;
  }
  scheduleQueuedSelection(0);
}
function settlePendingMediaAction() {
  if (cameraStatus.state_code === MediaState.ERROR) {
    showError(cameraStatus.last_error || { message_key: "request_failed" }, "camera-state");
    pendingMediaAction = "";
    selectionInFlight = null;
    continueQueuedSelection();
    runQueuedRegion();
    return;
  }
  notices.clear("camera-state");
  const applied = sameSelection(selectionInFlight, statusSelection());
  if (pendingMediaAction === "start" && applied && cameraStatus.state_code === MediaState.RUNNING) {
    if (!queuedSelection) feedback("success", "cameraStarted", "camera-action", "running");
    pendingMediaAction = "";
    selectionInFlight = null;
    channelChoiceTouched = false;
    continueQueuedSelection();
    runQueuedRegion();
  } else if (pendingMediaAction === "start" && applied && cameraStatus.state_code === MediaState.WAITING_INPUT) {
    if (!queuedSelection) feedback("warning", "waitingInputNotice", "camera-action", "waiting_input");
    pendingMediaAction = "";
    selectionInFlight = null;
    channelChoiceTouched = false;
    continueQueuedSelection();
    runQueuedRegion();
  }
}
function clearAuthorization() {
  pendingClear = true;
  notices.clear("clear-action");
  const promise = gate.run("auth-clear", (operationId) => api.clearAuthorization(operationId)).then(() => schedulePoll(120)).catch((error) => {
    pendingClear = false;
    showError(error, "clear-action");
  }).finally(render);
  render();
  return promise;
}
function settlePendingClear() {
  if (!pendingClear) return;
  if (auth.state_code === AuthState.ERROR) {
    pendingClear = false;
    showError(auth.last_error || { message_key: "request_failed" }, "clear-action");
    return;
  }
  if (auth.state_code !== AuthState.IDLE) return;
  pendingClear = false;
  cameraStatus = { state_code: MediaState.STOPPED, selected: null, codec: null, last_error: null };
  cameras = [];
  cameraListState = CatalogState.IDLE;
  cameraRegion = "cn";
  pendingCameraRegion = null;
  cameraRenderKey = "";
  regionRenderKey = "";
  catalogLoaded = false;
  if (selectionTimer) window.clearTimeout(selectionTimer);
  selectionTimer = 0;
  selectionInFlight = null;
  queuedSelection = null;
  regionInFlight = null;
  queuedRegion = null;
  clearAuthorizationSecrets();
  resetChannelChoice();
  feedback("success", "authorizationCleared", "clear-action", "idle");
  queueMicrotask(() => ref("national-number")?.focus());
}
function settlePendingCancel() {
  if (!pendingCancel || [AuthState.WORKING, AuthState.SMS_REQUIRED].includes(auth.state_code)) return;
  pendingCancel = false;
  if (auth.state_code === AuthState.IDLE) {
    clearAuthorizationSecrets();
    feedback("success", "authorizationCanceled", "auth-cancel", "idle");
    queueMicrotask(() => ref("national-number")?.focus());
  }
}
function phoneErrorMessage(error) {
  if (!(error instanceof PhoneInputError)) return "invalidNationalNumber";
  if (error.code === "invalid_calling_code") return "invalidCallingCode";
  if (error.code === "phone_number_too_long") return "phoneNumberTooLong";
  return "invalidNationalNumber";
}

function bindEvents() {
  ref("rtsp-toggle").addEventListener("click", () => changeRtspSetting("enabled"));
  ref("rtsp-audio").addEventListener("click", () => changeRtspSetting("audio_enabled"));
  ref("rtsp-copy").addEventListener("click", () => loadRtspCredentials(true));
  ref("rtsp-retry").addEventListener("click", () => loadRtspCredentials());
  ref("rtsp-rotate").addEventListener("click", rotateRtspPassword);
  clearDialog = createDialogController(ref("clear-dialog"), {
    initialFocus: () => app.querySelector("[data-action='clear-cancel']"),
    onClose: (reason) => { if (reason === "confirm") void clearAuthorization(); }
  });
  for (const name of ["national-number", "sms-code"]) ref(name).addEventListener("input", (event) => clearInvalid(event.currentTarget));
  ref("calling-code-trigger").addEventListener("click", () => {
    setCountryPickerOpen(!countryPickerOpen, countryPickerOpen ? "" : "selected");
  });
  ref("calling-code-trigger").addEventListener("keydown", handleCountryTriggerKeydown);
  ref("calling-code-listbox").addEventListener("keydown", handleCountryListboxKeydown);
  ref("calling-code-listbox").addEventListener("click", (event) => {
    const option = event.target.closest("[role='option']");
    if (option) selectCountry(option.dataset.countryKey);
  });
  document.addEventListener("pointerdown", (event) => {
    if (countryPickerOpen && !ref("country-picker").contains(event.target)) {
      setCountryPickerOpen(false);
    }
  });
  app.querySelector("[data-form='phone']").addEventListener("submit", (event) => {
    event.preventDefault();
    if (auth.account || [AuthState.REAUTH_REQUIRED, AuthState.RETRYING].includes(auth.state_code)) return;
    if (auth.state_code === AuthState.WORKING) return;
    if (auth.state_code === AuthState.SMS_REQUIRED && auth.challenge?.kind === "sms") {
      if (auth.challenge.retry_after_seconds > 0) return;
      pendingAuthAction = "resend";
      void runAuthorizationMutation("auth-sms-resend", (operationId) => api.resendSms(operationId));
      return;
    }
    let phone;
    try {
      phone = normalizePhoneFields(
        callingCodeForCountryKey(ref("calling-code").value),
        ref("national-number").value
      );
    } catch (error) {
      const messageKey = phoneErrorMessage(error);
      feedback("warning", messageKey);
      focusInvalid(messageKey === "invalidCallingCode" ? ref("calling-code-trigger") : ref("national-number"));
      return;
    }
    clearInvalid(ref("calling-code-trigger"));
    clearInvalid(ref("national-number"));
    notices.clear("auth-step");
    notices.clear("auth-terminal");
    notices.clear("auth-cancel");
    authAttemptActive = true;
    pendingAuthAction = "start";
    void runAuthorizationMutation("auth-sms-start", (operationId) => api.startSms(operationId, phone.calling_code, phone.national_number));
  });
  app.querySelector("[data-form='sms']").addEventListener("submit", (event) => {
    event.preventDefault();
    const challenge = auth?.challenge;
    const submittedCode = consumeSecret(ref("sms-code"), true);
    let code;
    try {
      code = normalizeSmsCode(submittedCode, challenge?.code_length);
    } catch {
      feedback("warning", "invalidSmsCode", "", "", { length: challenge?.code_length || "-" });
      focusInvalid(ref("sms-code"));
      return;
    }
    clearInvalid(ref("sms-code"));
    pendingAuthAction = "verify";
    void runAuthorizationMutation("auth-sms-verify", (operationId) => api.verifySms(operationId, code));
  });
  app.querySelector("[data-action='auth-cancel']").addEventListener("click", () => { void cancelAuthorizationAttempt(); });
  app.querySelector("[data-action='clear']").addEventListener("click", (event) => clearDialog.open(event.currentTarget));
  app.querySelector("[data-action='clear-cancel']").addEventListener("click", () => clearDialog.close("cancel"));
  app.querySelector("[data-action='clear-confirm']").addEventListener("click", () => clearDialog.close("confirm"));
  ref("camera-region").addEventListener("change", queueRegionFromControl);
  ref("camera-select").addEventListener("change", queueSelectionFromControls);
  ref("channel-field").addEventListener("change", () => {
    channelChoiceTouched = true;
    queueSelectionFromControls();
  });
}

async function refreshStatus(showFailure = false) {
  if (pollInFlight || document.visibilityState === "hidden") return;
  pollInFlight = true;
  renderBusy();
  try {
    const [authResult, cameraResult] = await Promise.all([api.authStatus(), api.cameraStatus(), refreshRtsp()]);
    projectAuth(authResult);
    projectCameraStatus(cameraResult);
    notices.clear("status-poll");
    const authStatusError = authPollError(auth);
    const mediaStatusError = cameraStatus.state_code === MediaState.ERROR
      ? cameraStatus.last_error || { message_key: "request_failed" }
      : null;
    const newAuthStatusError = statusErrors.observe("auth", authStatusError);
    const newMediaStatusError = statusErrors.observe("media", mediaStatusError);
    if (newAuthStatusError && !authAttemptActive && !pendingCancel && !pendingClear) {
      showError(authStatusError, "auth-poll");
    } else if (!authStatusError) notices.clear("auth-poll");
    if (newMediaStatusError && !pendingMediaAction) {
      showError(mediaStatusError, "camera-state");
    } else if (!mediaStatusError) notices.clear("camera-state");
    settlePendingClear();
    settlePendingCancel();
    settleAuthorizationFlow();
    settlePendingMediaAction();
    render();
    if (
      auth.state_code === AuthState.AUTHENTICATED &&
      !authAttemptActive && !pendingClear && !regionInFlight &&
      !catalogPromise && !catalogLoaded && !CATALOG_ACTIVE_STATES.has(cameraListState)
    ) void loadCameraList(false).catch(() => {});
    statusErrors.finishHydration();
    initialStatusLoaded = true;
  } catch (error) {
    if (showFailure || initialStatusLoaded) showError(error, "status-poll");
    else feedback("error", "statusUnavailable", "status-poll", errorSignature(error));
  } finally {
    pollInFlight = false;
    renderBusy();
    const authChallengeActive = auth.state_code === AuthState.SMS_REQUIRED;
    const active = authAttemptActive || pendingCancel || authChallengeActive || pendingClear || Boolean(pendingMediaAction) || Boolean(regionInFlight) || [AuthState.WORKING, AuthState.RETRYING].includes(auth.state_code) || cameraListState === CatalogState.RETRYING || cameraStatus.state_code === MediaState.STARTING;
    schedulePoll(active ? 800 : 5000);
  }
}
function schedulePoll(delay) {
  if (pollTimer) window.clearTimeout(pollTimer);
  pollTimer = window.setTimeout(() => void refreshStatus(false), delay);
}
Locale.addListener((next) => { language = next.language; applyLocale(); });
document.addEventListener("visibilitychange", () => {
  if (document.visibilityState === "hidden") {
    rtspCredentialsNeedLoad = true;
    clearRtspCredentials();
    if (pollTimer) window.clearTimeout(pollTimer);
    pollTimer = 0;
    return;
  }
  void refreshStatus(false);
  if (rtspCredentialsNeedLoad) loadRtspCredentials();
});

mount();
window.AIniceUI.ready();
void refreshStatus(true);
loadRtspCredentials();
