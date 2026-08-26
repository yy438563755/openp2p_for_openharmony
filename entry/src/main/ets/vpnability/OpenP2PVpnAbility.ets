import { Want } from '@kit.AbilityKit';
import { vpnExtension, VpnExtensionAbility } from '@kit.NetworkKit';
import { connection } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';
import { OpenP2PEngine } from '../model/OpenP2PEngine';
import { EngineStatus, SessionStore } from '../model/SessionStore';

const DOMAIN = 0x4F50;
const TAG = 'OpenP2PVpn';

interface SdwanNode {
  name: string;
  ip: string;
  resource?: string;
}

interface SdwanInfo {
  id?: number;
  name?: string;
  gateway?: string;
  mtu?: number;
  Nodes?: SdwanNode[];
}

interface GatewayNetwork {
  ip: string;
  prefix: number;
}

/** VPN process exclusively owns Go, the TUN fd, and the native pump. */
export default class OpenP2PVpnAbility extends VpnExtensionAbility {
  private vpnConnection: vpnExtension.VpnConnection | undefined = undefined;
  private tunFd: number = -1;
  private running: boolean = false;
  private baseDir: string = '';
  private token: string = '';
  private lastSdwanJson: string = '';

  onCreate(want: Want): void {
    hilog.info(DOMAIN, TAG, 'VpnAbility onCreate');
    this.baseDir = this.context.filesDir;
    const params = want.parameters;
    let tokenFromWant = '';
    if (params !== undefined) {
      if (params['token'] !== undefined) {
        tokenFromWant = params['token'] as string;
      }
    }
    this.running = true;
    this.vpnConnection = vpnExtension.createVpnConnection(this.context);
    this.writeStatus('starting', false, '正在启动 VPN 内核…');
    this.startStatusLoop();
    setTimeout(() => {
      this.bootstrap(tokenFromWant);
    }, 0);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'VpnAbility onDestroy');
    this.running = false;
    this.closeTun();
    this.writeStatus('stopped', false, 'VPN 已停止');
    try {
      // StopModule exits only the dedicated :vpn process.
      OpenP2PEngine.stop();
    } catch (_e) {
    }
  }

  private async bootstrap(tokenFromWant: string): Promise<void> {
    const tokenFromStore = await SessionStore.loadToken(this.context);
    this.token = (tokenFromWant && tokenFromWant.length > 0) ? tokenFromWant : tokenFromStore;
    hilog.info(DOMAIN, TAG, 'bootstrap VPN-owned engine tokenLen=%{public}d', this.token.length);

    if (!OpenP2PEngine.isStarted()) {
      if (!this.token || this.token.length === 0 || this.token === '0') {
        hilog.error(DOMAIN, TAG, 'empty token, cannot runAsModule');
        this.writeStatus('error', false, 'Token 为空');
        return;
      }
      this.writeStatus('connecting', false, '正在连接控制台…');
      const ok = await OpenP2PEngine.runAsModule(this.baseDir, this.token, 0, 1);
      hilog.info(DOMAIN, TAG, 'engine start in vpn process: %{public}s lastError=%{public}s',
        String(ok), OpenP2PEngine.lastError());
      if (!ok) {
        const detail = OpenP2PEngine.lastError();
        this.writeStatus('error', false, detail.length > 0 ? detail : 'VPN 进程登录失败');
        return;
      }
    }
    this.writeStatus('online', OpenP2PEngine.isOnline(), '控制台已连接，等待 SD-WAN…');
    await this.loopSdwanAndTun();
  }

  private async loopSdwanAndTun(): Promise<void> {
    while (this.running) {
      try {
        hilog.info(DOMAIN, TAG, 'waiting SD-WAN config...');
        const cfgBuf = await OpenP2PEngine.getSdwanConfigAsync();
        const decoder = util.TextDecoder.create('utf-8');
        const jsonStr = decoder.decodeToString(new Uint8Array(cfgBuf));
        if (jsonStr.length === 0) {
          hilog.warn(DOMAIN, TAG, 'empty SD-WAN config, retry');
          await this.sleep(1000);
          continue;
        }
        if (jsonStr === this.lastSdwanJson && this.tunFd >= 0) {
          hilog.info(DOMAIN, TAG, 'same SD-WAN config, keep existing TUN fd=%{public}d', this.tunFd);
          continue;
        }
        hilog.info(DOMAIN, TAG, 'got SD-WAN config len=%{public}d', jsonStr.length);
        this.closeTun();
        await this.sleep(300);
        await this.setupVpn(jsonStr);
        if (this.tunFd < 0) {
          await this.sleep(1000);
          continue;
        }
        const pumpOk = OpenP2PEngine.startTunPump(this.tunFd);
        hilog.info(DOMAIN, TAG, 'tun pump started=%{public}s tunFd=%{public}d', String(pumpOk), this.tunFd);
        this.writeStatus(pumpOk ? 'ready' : 'error', OpenP2PEngine.isOnline(),
          pumpOk ? 'VPN 数据面就绪' : 'TUN pump 启动失败');
        this.lastSdwanJson = jsonStr;
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'sdwan/tun loop error: %{public}s', JSON.stringify(e));
        this.lastSdwanJson = '';
        await this.sleep(1000);
      }
    }
  }

  private readTextFile(name: string): string {
    try {
      const path = this.baseDir.endsWith('/') ? this.baseDir + name : this.baseDir + '/' + name;
      if (!fs.accessSync(path)) {
        return '';
      }
      const stat = fs.statSync(path);
      const file = fs.openSync(path, fs.OpenMode.READ_ONLY);
      const buf = new ArrayBuffer(stat.size);
      fs.readSync(file.fd, buf);
      fs.closeSync(file);
      return util.TextDecoder.create('utf-8', { ignoreBOM: true }).decodeWithStream(new Uint8Array(buf));
    } catch (_e) {
      return '';
    }
  }

  private makeNetAddress(ip: string): connection.NetAddress {
    return { address: ip, family: 1, port: 0 };
  }

  private makeLinkAddress(ip: string, prefixLength: number): vpnExtension.LinkAddress {
    return { address: this.makeNetAddress(ip), prefixLength: prefixLength };
  }

  private makeRoute(destIp: string, prefixLength: number): vpnExtension.RouteInfo {
    return {
      interface: 'vpn-tun',
      destination: this.makeLinkAddress(destIp, prefixLength),
      gateway: this.makeNetAddress('0.0.0.0'),
      hasGateway: false,
      isDefaultRoute: false
    };
  }

  private async setupVpn(jsonStr: string): Promise<void> {
    if (!this.vpnConnection) {
      throw new Error('vpnConnection is null');
    }
    const info = JSON.parse(jsonStr) as SdwanInfo;
    let myName = '';
    try {
      myName = OpenP2PEngine.getNodeName();
    } catch (_e) {
      myName = '';
    }
    if (!myName || myName.length === 0) {
      myName = this.readTextFile('node_name.txt').trim();
    }
    const mtu = info.mtu && info.mtu > 0 ? info.mtu : 1420;

    const addresses: Array<vpnExtension.LinkAddress> = [];
    const routes: Array<vpnExtension.RouteInfo> = [];
    const nodes = info.Nodes ?? [];
    for (let i = 0; i < nodes.length; i++) {
      const node = nodes[i];
      if (node.name === myName && node.ip) {
        addresses.push(this.makeLinkAddress(node.ip, 24));
      }
      if (node.resource) {
        const parts = node.resource.split(',');
        for (let j = 0; j < parts.length; j++) {
          const cidr = parts[j].trim();
          const segs = cidr.split('/');
          if (segs.length === 2) {
            routes.push(this.makeRoute(segs[0].trim(), Number(segs[1])));
          }
        }
      }
    }
    const gwNet = this.networkFromGateway(info.gateway ?? '');
    if (gwNet !== undefined) {
      routes.push(this.makeRoute(gwNet.ip, gwNet.prefix));
    }
    if (addresses.length === 0) {
      hilog.error(DOMAIN, TAG, 'no address for node=%{public}s, skip vpn create', myName);
      return;
    }
    const config: vpnExtension.VpnConfig = {
      addresses: addresses,
      routes: routes,
      dnsAddresses: ['119.29.29.29', '2400:3200::1'],
      mtu: mtu
    };
    this.tunFd = await this.vpnConnection.create(config);
    hilog.info(DOMAIN, TAG, 'vpn create tunFd=%{public}d addr=%{public}s', this.tunFd, addresses[0].address.address);
  }

  private closeTun(): void {
    try {
      OpenP2PEngine.stopTunPump();
    } catch (_e) {
    }
    if (this.tunFd >= 0) {
      try {
        fs.closeSync(this.tunFd);
      } catch (_e) {
      }
      this.tunFd = -1;
    }
    try {
      this.vpnConnection?.destroy();
    } catch (_e) {
    }
    try {
      this.vpnConnection = vpnExtension.createVpnConnection(this.context);
    } catch (_e) {
    }
  }

  private networkFromGateway(cidr: string): GatewayNetwork | undefined {
    const parts = cidr.trim().split('/');
    if (parts.length !== 2) {
      return undefined;
    }
    const prefix = Number(parts[1]);
    if (isNaN(prefix) || prefix < 0 || prefix > 32) {
      return undefined;
    }
    const octets = parts[0].split('.');
    if (octets.length !== 4) {
      return undefined;
    }
    const o0 = Number(octets[0]);
    const o1 = Number(octets[1]);
    const o2 = Number(octets[2]);
    const o3 = Number(octets[3]);
    const ipInt = ((o0 << 24) | (o1 << 16) | (o2 << 8) | o3) >>> 0;
    const mask = prefix === 0 ? 0 : (0xffffffff << (32 - prefix)) >>> 0;
    const netInt = (ipInt & mask) >>> 0;
    const ip = [
      (netInt >>> 24) & 0xff,
      (netInt >>> 16) & 0xff,
      (netInt >>> 8) & 0xff,
      netInt & 0xff
    ].join('.');
    return { ip: ip, prefix: prefix };
  }

  private sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }

  private writeStatus(state: string, online: boolean, detail: string): void {
    let node = '';
    if (OpenP2PEngine.isStarted()) {
      node = OpenP2PEngine.getNodeName();
    }
    const status: EngineStatus = {
      state: state,
      online: online,
      detail: detail,
      node: node,
      tunFd: this.tunFd,
      probe: OpenP2PEngine.isStarted() ? OpenP2PEngine.getProbeStatus() : '尚未探测',
      updatedAt: Date.now()
    };
    SessionStore.saveStatus(this.context, status);
  }

  private startStatusLoop(): void {
    const tick = (): void => {
      if (!this.running) {
        return;
      }
      const online = OpenP2PEngine.isOnline();
      const detail = online
        ? (this.tunFd >= 0 ? 'VPN 数据面就绪' : '控制台在线，等待虚拟组网…')
        : 'VPN 进程正在连接控制台…';
      this.writeStatus(this.tunFd >= 0 ? 'ready' : (online ? 'online' : 'connecting'), online, detail);
      setTimeout(tick, 2000);
    };
    setTimeout(tick, 500);
  }
}
