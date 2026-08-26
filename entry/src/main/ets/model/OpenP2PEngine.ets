import openp2p from 'libentry.so';
import { hilog } from '@kit.PerformanceAnalysisKit';

const DOMAIN = 0x4F50; // 'OP'
const TAG = 'OpenP2PEngine';

/**
 * Thin wrapper around the Go core NAPI exports.
 * Blocking SD-WAN / TUN helpers should be called off the UI thread.
 * runAsModule is async (NAPI Promise) so UI stays responsive during login.
 */
export class OpenP2PEngine {
  private static started: boolean = false;

  static isStarted(): boolean {
    return OpenP2PEngine.started;
  }

  static nativeStatus(): string {
    try {
      return openp2p.nativeStatus();
    } catch (e) {
      return 'libentry.so not loaded: ' + JSON.stringify(e);
    }
  }

  static lastError(): string {
    try {
      return openp2p.lastError();
    } catch (e) {
      return '';
    }
  }

  static getToken(baseDir: string): string {
    try {
      return openp2p.getToken(baseDir);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'getToken failed: %{public}s', JSON.stringify(e));
      return '0';
    }
  }

  static async runAsModule(baseDir: string, token: string, bw: number = 0, logLevel: number = 1): Promise<boolean> {
    try {
      const status = OpenP2PEngine.nativeStatus();
      hilog.info(DOMAIN, TAG, 'nativeStatus=%{public}s', status);
      // Compatible with both Promise (async NAPI) and plain boolean (old so)
      const ok = await Promise.resolve(openp2p.runAsModule(baseDir, token, bw, logLevel));
      OpenP2PEngine.started = ok;
      hilog.info(DOMAIN, TAG, 'runAsModule ok=%{public}s', String(ok));
      return ok;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'runAsModule failed: %{public}s', JSON.stringify(e));
      OpenP2PEngine.started = false;
      return false;
    }
  }

  static connect(timeoutMs: number = 1000): boolean {
    try {
      return openp2p.connect(timeoutMs);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'connect failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  /** Non-blocking online check for UI polling. */
  static isOnline(): boolean {
    try {
      return openp2p.isOnline();
    } catch (e) {
      return false;
    }
  }

  static getNodeName(): string {
    try {
      return openp2p.getNodeName();
    } catch (e) {
      return '';
    }
  }

  static getProbeStatus(): string {
    try {
      return openp2p.getProbeStatus();
    } catch (_e) {
      return '探测接口未就绪';
    }
  }

  /** Blocking until a new SD-WAN config arrives. Do not use on main thread. */
  static getSdwanConfig(): ArrayBuffer {
    return openp2p.getSdwanConfig();
  }

  static async getSdwanConfigAsync(): Promise<ArrayBuffer> {
    return await Promise.resolve(openp2p.getSdwanConfigAsync());
  }

  static ohosRead(pkt: ArrayBuffer | Uint8Array): void {
    openp2p.ohosRead(pkt);
  }

  static ohosWrite(timeoutMs: number = 3000, maxLen: number = 4096): ArrayBuffer {
    return openp2p.ohosWrite(timeoutMs, maxLen);
  }

  static startTunPump(tunFd: number): boolean {
    try {
      return openp2p.startTunPump(tunFd);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'startTunPump failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  static stopTunPump(): void {
    try {
      openp2p.stopTunPump();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'stopTunPump failed: %{public}s', JSON.stringify(e));
    }
  }

  /** UI: AF_UNIX host bridging VPN TUN packets into Go. */
  static startTunBridgeHost(sockPath: string): boolean {
    try {
      return openp2p.startTunBridgeHost(sockPath);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'startTunBridgeHost failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  /** VPN: AF_UNIX device bridging local TUN to UI Go. */
  static startTunBridgeDevice(sockPath: string, tunFd: number): boolean {
    try {
      return openp2p.startTunBridgeDevice(sockPath, tunFd);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'startTunBridgeDevice failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  static stopTunBridge(): void {
    try {
      openp2p.stopTunBridge();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'stopTunBridge failed: %{public}s', JSON.stringify(e));
    }
  }

  static stop(): void {
    try {
      openp2p.stopTunBridge();
      openp2p.stopTunPump();
      openp2p.stop();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'stop failed: %{public}s', JSON.stringify(e));
    }
    OpenP2PEngine.started = false;
  }

  /** UI-only flag reset */
  static markStopped(): void {
    OpenP2PEngine.started = false;
  }
}
