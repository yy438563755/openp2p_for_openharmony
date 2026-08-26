import { common } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';

const DOMAIN = 0x4F50;
const TAG = 'SessionStore';
const SESSION_FILE = 'openp2p_session.json';
const STATUS_FILE = 'openp2p_status.json';

export interface EngineStatus {
  state: string;
  online: boolean;
  detail: string;
  node: string;
  tunFd: number;
  probe: string;
  updatedAt: number;
}

/**
 * Share login token between UIAbility and VpnExtensionAbility via filesDir.
 */
export class SessionStore {
  private static filePath(context: common.Context): string {
    const baseDir = context.filesDir;
    return baseDir.endsWith('/') ? baseDir + SESSION_FILE : baseDir + '/' + SESSION_FILE;
  }

  static async save(context: common.Context, token: string): Promise<boolean> {
    try {
      const path = SessionStore.filePath(context);
      const text = JSON.stringify({ 'token': token });
      const uint8 = new util.TextEncoder().encodeInto(text);
      const file = fs.openSync(path, fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, uint8.buffer);
      fs.closeSync(file);
      return true;
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'save failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  static async loadToken(context: common.Context): Promise<string> {
    try {
      const path = SessionStore.filePath(context);
      if (!fs.accessSync(path)) {
        return '';
      }
      const stat = fs.statSync(path);
      const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const buf = new ArrayBuffer(stat.size);
      fs.readSync(file.fd, buf);
      fs.closeSync(file);
      const text = util.TextDecoder.create('utf-8', { ignoreBOM: true }).decodeWithStream(new Uint8Array(buf));
      const data = JSON.parse(text) as Record<string, string>;
      return data['token'] ?? '';
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'load failed: %{public}s', JSON.stringify(e));
      return '';
    }
  }

  private static statusPath(context: common.Context): string {
    const baseDir = context.filesDir;
    return baseDir.endsWith('/') ? baseDir + STATUS_FILE : baseDir + '/' + STATUS_FILE;
  }

  /** Cross-process status channel: VPN writes, UI reads. */
  static saveStatus(context: common.Context, status: EngineStatus): boolean {
    try {
      const text = JSON.stringify(status);
      const uint8 = new util.TextEncoder().encodeInto(text);
      const file = fs.openSync(SessionStore.statusPath(context),
        fs.OpenMode.READ_WRITE | fs.OpenMode.CREATE | fs.OpenMode.TRUNC);
      fs.writeSync(file.fd, uint8.buffer);
      fs.closeSync(file);
      return true;
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'saveStatus failed: %{public}s', JSON.stringify(e));
      return false;
    }
  }

  static loadStatus(context: common.Context): EngineStatus | undefined {
    try {
      const path = SessionStore.statusPath(context);
      if (!fs.accessSync(path)) {
        return undefined;
      }
      const stat = fs.statSync(path);
      const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const buf = new ArrayBuffer(stat.size);
      fs.readSync(file.fd, buf);
      fs.closeSync(file);
      const text = util.TextDecoder.create('utf-8', { ignoreBOM: true })
        .decodeWithStream(new Uint8Array(buf));
      return JSON.parse(text) as EngineStatus;
    } catch (_e) {
      // Writer may be replacing the small JSON file; next poll will retry.
      return undefined;
    }
  }

  static clearStatus(context: common.Context): void {
    try {
      const path = SessionStore.statusPath(context);
      if (fs.accessSync(path)) {
        fs.unlinkSync(path);
      }
    } catch (_e) {
    }
  }

  static async clear(context: common.Context): Promise<void> {
    try {
      const path = SessionStore.filePath(context);
      if (fs.accessSync(path)) {
        fs.unlinkSync(path);
      }
    } catch (_e) {
    }
  }
}
