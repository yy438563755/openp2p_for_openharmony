/** Returns Promise — login blocks Go Connect, must not run on UI thread. */
export const runAsModule: (baseDir: string, token: string, bw?: number, logLevel?: number) => Promise<boolean>;
export const getToken: (baseDir: string) => string;
export const connect: (timeoutMs?: number) => boolean;
/** Non-blocking heartbeat check. */
export const isOnline: () => boolean;
export const stop: () => void;
export const getNodeName: () => string;
/** Latest Go-native SD-WAN ICMP probe result. */
export const getProbeStatus: () => string;
/** Blocking — do not call on ArkTS main thread. */
export const getSdwanConfig: () => ArrayBuffer;
/** Promise — waits for SD-WAN JSON on a worker thread. */
export const getSdwanConfigAsync: () => Promise<ArrayBuffer>;
export const ohosRead: (pkt: ArrayBuffer | Uint8Array) => void;
/** Blocking with timeout. Returns one packet or empty buffer. */
export const ohosWrite: (timeoutMs?: number, maxLen?: number) => ArrayBuffer;
/** Start background TUN pump (dup fd, C++ thread). */
export const startTunPump: (tunFd: number) => boolean;
export const stopTunPump: () => void;
/** UI process: listen AF_UNIX and bridge to Go OhosRead/Write. */
export const startTunBridgeHost: (sockPath: string) => boolean;
/** VPN process: connect AF_UNIX and bridge TUN <-> UI Go. */
export const startTunBridgeDevice: (sockPath: string, tunFd: number) => boolean;
export const stopTunBridge: () => void;
/** "ok" or dlopen/dlsym error string */
export const nativeStatus: () => string;
/** Last RunAsModule failure reason */
export const lastError: () => string;
