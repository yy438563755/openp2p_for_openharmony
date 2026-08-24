/** Returns Promise — login blocks Go Connect, must not run on UI thread. */
export const runAsModule: (baseDir: string, token: string, bw?: number, logLevel?: number) => Promise<boolean>;
export const getToken: (baseDir: string) => string;
export const connect: (timeoutMs?: number) => boolean;
/** Non-blocking heartbeat check. */
export const isOnline: () => boolean;
export const stop: () => void;
export const getNodeName: () => string;
/** Blocking — call from VPN worker loop. Returns SD-WAN JSON bytes. */
export const getSdwanConfig: () => ArrayBuffer;
export const ohosRead: (pkt: ArrayBuffer | Uint8Array) => void;
/** Blocking with timeout. Returns one packet or empty buffer. */
export const ohosWrite: (timeoutMs?: number, maxLen?: number) => ArrayBuffer;
/** "ok" or dlopen/dlsym error string */
export const nativeStatus: () => string;
