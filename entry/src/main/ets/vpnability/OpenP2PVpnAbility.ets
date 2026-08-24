import { Want } from '@kit.AbilityKit';
import { vpnExtension, VpnExtensionAbility } from '@kit.NetworkKit';
import { connection } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { fileIo as fs } from '@kit.CoreFileKit';
import { util } from '@kit.ArkTS';
import { OpenP2PEngine } from '../model/OpenP2PEngine';

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

export default class OpenP2PVpnAbility extends VpnExtensionAbility {
  private vpnConnection: vpnExtension.VpnConnection | undefined = undefined;
  private tunFd: number = -1;
  private running: boolean = false;
  private baseDir: string = '';
  private token: string = '';

  onCreate(want: Want): void {
    hilog.info(DOMAIN, TAG, 'VpnAbility onCreate');
    const params = want.parameters;
    if (params !== undefined && params['baseDir'] !== undefined) {
      this.baseDir = params['baseDir'] as string;
    } else {
      this.baseDir = this.context.filesDir;
    }
    if (params !== undefined && params['token'] !== undefined) {
      this.token = params['token'] as string;
    } else {
      this.token = '';
    }
    this.vpnConnection = vpnExtension.createVpnConnection(this.context);

    this.running = true;
    setTimeout(() => {
      this.ensureEngineThenLoop();
    }, 0);
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'VpnAbility onDestroy');
    this.running = false;
    this.closeTun();
    try {
      OpenP2PEngine.stop();
    } catch (_e) {
    }
  }

  private async ensureEngineThenLoop(): Promise<void> {
    if (!OpenP2PEngine.isStarted()) {
      const ok = await OpenP2PEngine.runAsModule(this.baseDir, this.token, 0, 1);
      hilog.info(DOMAIN, TAG, 'engine start in vpn process: %{public}s', String(ok));
    }
    await this.loopSdwanAndTun();
  }

  private async loopSdwanAndTun(): Promise<void> {
    while (this.running) {
      try {
        hilog.info(DOMAIN, TAG, 'waiting SD-WAN config...');
        const cfgBuf = OpenP2PEngine.getSdwanConfig();
        const decoder = util.TextDecoder.create('utf-8');
        const jsonStr = decoder.decodeToString(new Uint8Array(cfgBuf));
        hilog.info(DOMAIN, TAG, 'got SD-WAN config len=%{public}d', jsonStr.length);
        this.closeTun();
        await this.setupVpn(jsonStr);
        await this.pumpTun();
      } catch (e) {
        hilog.error(DOMAIN, TAG, 'sdwan/tun loop error: %{public}s', JSON.stringify(e));
        await this.sleep(1000);
      }
    }
  }

  private makeNetAddress(ip: string): connection.NetAddress {
    const addr: connection.NetAddress = {
      address: ip,
      family: 1,
      port: 0
    };
    return addr;
  }

  private makeLinkAddress(ip: string, prefixLength: number): vpnExtension.LinkAddress {
    const link: vpnExtension.LinkAddress = {
      address: this.makeNetAddress(ip),
      prefixLength: prefixLength
    };
    return link;
  }

  private makeRoute(destIp: string, prefixLength: number): vpnExtension.RouteInfo {
    const route: vpnExtension.RouteInfo = {
      interface: 'vpn-tun',
      destination: this.makeLinkAddress(destIp, prefixLength),
      gateway: this.makeNetAddress('0.0.0.0'),
      hasGateway: false,
      isDefaultRoute: false
    };
    return route;
  }

  private async setupVpn(jsonStr: string): Promise<void> {
    if (!this.vpnConnection) {
      throw new Error('vpnConnection is null');
    }
    const info = JSON.parse(jsonStr) as SdwanInfo;
    const myName = OpenP2PEngine.getNodeName();
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

    const config: vpnExtension.VpnConfig = {
      addresses: addresses,
      routes: routes,
      dnsAddresses: ['119.29.29.29', '2400:3200::1'],
      mtu: mtu
    };

    this.tunFd = await this.vpnConnection.create(config);
    hilog.info(DOMAIN, TAG, 'vpn create tunFd=%{public}d', this.tunFd);
  }

  private async pumpTun(): Promise<void> {
    const readBuf = new ArrayBuffer(4096);
    while (this.running && this.tunFd >= 0) {
      try {
        const n = fs.readSync(this.tunFd, readBuf);
        if (n > 0) {
          OpenP2PEngine.ohosRead(readBuf.slice(0, n));
        }
      } catch (_e) {
        // no data
      }

      const out = OpenP2PEngine.ohosWrite(50, 4096);
      if (out.byteLength > 0) {
        try {
          fs.writeSync(this.tunFd, out);
        } catch (e) {
          hilog.error(DOMAIN, TAG, 'tun write failed: %{public}s', JSON.stringify(e));
        }
      }
    }
  }

  private closeTun(): void {
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
    const result: GatewayNetwork = {
      ip: ip,
      prefix: prefix
    };
    return result;
  }

  private sleep(ms: number): Promise<void> {
    return new Promise((resolve) => setTimeout(resolve, ms));
  }
}
