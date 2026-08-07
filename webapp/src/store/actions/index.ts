// Barrel for the action modules. This preserves the historical import path
// `store/actions` (used by components, tests, and vi.mock calls) while the
// implementation lives in per-seam modules. The re-export list is exactly the
// export surface the old single-file actions.ts had: internals that other
// action modules import directly (session, handleIncomingMessage, ...) are
// deliberately not re-exported here.
export {
  loadConnectionCapabilities,
  connect,
  disconnect,
} from './connection';
export {
  initMessageStore,
  mergeFirmwareMessages,
  loadMessages,
  registerBroadcastSendTelemetry,
  handleBroadcastDelivery,
  sendMessage,
  handleAck,
  normalizeIncomingRealtimeMessage,
  handleIncomingMessage,
  openDM,
  upsertProbeResponse,
  sendProbe,
  __resetBroadcastTelemetryForTests,
  __resetActionsForTests,
  __normalizeReplayDeliveryEventForTests,
  __clearDeliveryEventSyncStateForTests,
} from './messaging';
export type { FirmwareMergeContext } from './messaging';
export {
  normalizeConfig,
  loadConfig,
  saveRadio,
  saveNodeName,
  addChannel,
  removeChannel,
  setMailbox,
  setDefaultChannel,
  setLocationConfig,
  setGpsEnabled,
  loadTimezone,
  setTimezone,
  shareLocationOnce,
} from './config';
export {
  loadStatus,
  normalizeStatus,
  normalizeAirtime,
  loadAirtime,
  loadNeighbors,
  loadRoutes,
  loadPeerLocations,
  showOnMap,
  loadTrafficDebugStatus,
  setTrafficDebugConfig,
  decodePacketType,
  loadTrafficEvents,
} from './telemetry';
export {
  loadPeerVerification,
  setPeerVerified,
  setNetworkKey,
  generateNetworkKey,
  loadNetworkKeyStatus,
  setAnchor,
  getIdentity,
  setEndorsement,
  loadAnchorStatus,
} from './security';
export {
  getAuthToken,
  setAuthToken,
  getAllowedOrigins,
  setAllowedOrigins,
  getOtaOrigin,
  setOtaOrigin,
  resetOtaOrigin,
  startOtaUpdate,
  getOtaStatus,
  subscribeOtaEvents,
} from './deviceManagement';
export type { AuthTokenInfo, OtaOriginInfo, OtaStatus } from './deviceManagement';
export {
  saveConnectedDevice,
  refreshDevices,
  forgetSavedDevice,
  renameSavedDevice,
} from './deviceBook';
