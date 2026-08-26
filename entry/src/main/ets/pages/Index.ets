import { common, Want } from '@kit.AbilityKit';
import { vpnExtension } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { router } from '@kit.ArkUI';
import { SessionStore } from '../model/SessionStore';

const DOMAIN = 0x4F50;
const TAG = 'Index';
const CONSOLE_URL = 'https://console.openp2p.cn/profile';

@Entry
@Component
struct Index {
  @State token: string = '';
  @State onlineState: string = '未登录';
  @State loginLabel: string = '登录';
  @State helpText: string =
    'Token 可在 https://console.openp2p.cn/profile 获取。\n\n' +
    '鸿蒙提示：请在系统设置中允许本应用后台运行与联网，避免进程被清理后掉线。\n' +
    '登录成功后将启动 VPN 扩展以承载 SD-WAN 流量。';
  @State busy: boolean = false;
  private baseDir: string = '';
  private hostContext: common.UIAbilityContext | undefined = undefined;
  private bundleName: string = 'com.openp2p.client';
  private polling: boolean = false;
  private loggedIn: boolean = false;

  aboutToAppear(): void {
    const host = this.getUIContext().getHostContext();
    if (host) {
      const ctx = host as common.UIAbilityContext;
      this.hostContext = ctx;
      this.baseDir = ctx.filesDir;
      try {
        this.bundleName = ctx.abilityInfo.bundleName;
      } catch (_e) {
        this.bundleName = 'com.openp2p.client';
      }
    }
    setTimeout(() => {
      this.prefetchToken();
    }, 50);
  }

  aboutToDisappear(): void {
    this.polling = false;
  }

  private prefetchToken(): void {
    if (this.hostContext === undefined) {
      return;
    }
    SessionStore.loadToken(this.hostContext).then((saved: string) => {
      if (saved && saved !== '0' && saved.length > 0) {
        this.token = saved;
      }
    }).catch((e: Object) => {
      hilog.warn(DOMAIN, TAG, 'prefetchToken failed: %{public}s', JSON.stringify(e));
    });
  }

  build() {
    Column({ space: 12 }) {
      Text('OpenP2P')
        .fontSize(28)
        .fontWeight(FontWeight.Bold)
        .width('100%')

      Row({ space: 8 }) {
        TextInput({ placeholder: 'Token', text: this.token })
          .layoutWeight(1)
          .height(44)
          .enabled(!this.loggedIn)
          .onChange((v: string) => {
            this.token = v;
          })

        Button(this.loginLabel)
          .height(44)
          .enabled(this.loggedIn || (!this.busy && this.token.length > 0))
          .onClick(() => {
            this.onLoginClick();
          })
      }
      .width('100%')

      Button('打开控制台查看 Token')
        .width('100%')
        .height(40)
        .onClick(() => {
          this.openConsole();
        })

      Text(this.onlineState)
        .fontSize(16)
        .fontColor(this.onlineState.startsWith('在线') ? Color.Green : Color.Grey)
        .width('100%')

      TextArea({ text: this.helpText })
        .layoutWeight(1)
        .width('100%')
        .enabled(false)
        .backgroundColor('#F5F5F5')
    }
    .padding(16)
    .width('100%')
    .height('100%')
  }

  private onLoginClick(): void {
    if (this.loggedIn) {
      this.doLogout();
      return;
    }
    this.doLogin();
  }

  private async doLogin(): Promise<void> {
    if (this.busy || this.loggedIn) {
      return;
    }
    this.busy = true;
    this.onlineState = '正在登录…';
    try {
      if (!this.baseDir || this.baseDir.length === 0 || this.hostContext === undefined) {
        this.onlineState = '登录失败：filesDir 为空';
        return;
      }
      let token = this.token.trim();
      // Keep digits only (ArkTS-safe)
      let digits = '';
      for (let i = 0; i < token.length; i++) {
        const ch = token.charAt(i);
        if (ch >= '0' && ch <= '9') {
          digits += ch;
        }
      }
      token = digits;
      if (token.length < 8) {
        this.onlineState = '登录失败：Token 无效（需为控制台数字 Token）';
        return;
      }
      this.token = token;
      try {
        await SessionStore.save(this.hostContext, token);
        SessionStore.clearStatus(this.hostContext);
      } catch (se) {
        hilog.warn(DOMAIN, TAG, 'session save ignored: %{public}s', JSON.stringify(se));
      }

      this.loggedIn = true;
      this.loginLabel = '退出';
      this.onlineState = '正在启动 VPN 内核…';
      this.startPolling();

      const vpnOk = await this.startVpnExtension();
      if (!vpnOk) {
        this.onlineState = 'VPN 启动失败';
        this.loggedIn = false;
        this.loginLabel = '登录';
        this.polling = false;
      }
    } catch (e) {
      const msg = this.errText(e);
      hilog.error(DOMAIN, TAG, 'login error: %{public}s', msg);
      this.onlineState = '登录异常: ' + msg;
      this.loggedIn = false;
      this.loginLabel = '登录';
    } finally {
      this.busy = false;
    }
  }

  private errText(e: Object): string {
    try {
      if (e instanceof Error) {
        return e.message;
      }
      const be = e as BusinessError;
      if (be !== undefined && be.code !== undefined) {
        return 'code=' + be.code + ' ' + (be.message ?? '');
      }
      return JSON.stringify(e);
    } catch (_e) {
      return String(e);
    }
  }

  private doLogout(): void {
    hilog.info(DOMAIN, TAG, 'logout');
    this.polling = false;
    this.busy = false;
    this.loggedIn = false;
    this.loginLabel = '登录';
    this.onlineState = '已退出';
    if (this.hostContext !== undefined) {
      SessionStore.clear(this.hostContext);
      SessionStore.clearStatus(this.hostContext);
    }

    const stopWant: Want = {
      deviceId: '',
      bundleName: this.bundleName,
      abilityName: 'OpenP2PVpnAbility'
    };
    try {
      vpnExtension.stopVpnExtensionAbility(stopWant);
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'stopVpn failed: %{public}s', JSON.stringify(e));
    }
  }

  private startPolling(): void {
    if (this.polling) {
      return;
    }
    this.polling = true;
    const tick = (): void => {
      if (!this.polling || !this.loggedIn) {
        return;
      }
      if (this.hostContext !== undefined) {
        const status = SessionStore.loadStatus(this.hostContext);
        if (status !== undefined) {
          if (status.online && status.tunFd >= 0) {
            this.onlineState = '在线（VPN 数据面就绪）；' +
              (status.probe && status.probe.length > 0 ? status.probe : '尚未探测');
          } else if (status.online) {
            this.onlineState = '在线（正在创建虚拟网卡…）';
          } else if (status.state === 'error') {
            this.onlineState = '登录失败：' + status.detail;
          } else {
            this.onlineState = status.detail.length > 0 ? status.detail : '连接中…';
          }
        }
      }
      setTimeout(tick, 3000);
    };
    setTimeout(tick, 1000);
  }

  private async startVpnExtension(): Promise<boolean> {
    const want: Want = {
      deviceId: '',
      bundleName: this.bundleName,
      abilityName: 'OpenP2PVpnAbility',
      parameters: {
        'baseDir': this.baseDir,
        'token': this.token.trim(),
        // VPN 进程独占 Go + TUN；UI 禁止加载或调用 Go 引擎。
        'skipEngine': '0'
      }
    };
    hilog.info(DOMAIN, TAG, 'startVpn want bundle=%{public}s ability=OpenP2PVpnAbility', this.bundleName);
    try {
      await vpnExtension.startVpnExtensionAbility(want);
      hilog.info(DOMAIN, TAG, 'VPN extension started');
      return true;
    } catch (err) {
      const e = err as BusinessError;
      hilog.error(DOMAIN, TAG, 'startVpn failed code=%{public}d msg=%{public}s', e.code, e.message);
      const hint = (e.code === 2097152)
        ? '\n错误 2097152=系统 connectAbility 失败。可查：\n' +
          'hdc shell bm dump -n ' + this.bundleName + '\n' +
          'hdc shell param get const.product.supportVpn\n'
        : '\n';
      this.helpText =
        'VPN 扩展启动失败 code=' + e.code + ' ' + e.message + hint +
        '无法创建虚拟网卡。请允许 VPN 授权后重试。\n\n' + this.helpText;
      return false;
    }
  }

  private openConsole(): void {
    this.openConsoleWebView();
  }

  private openConsoleWebView(): void {
    hilog.info(DOMAIN, TAG, 'push ConsoleWebPage');
    router.pushUrl({
      url: 'pages/ConsoleWebPage',
      params: {
        'url': CONSOLE_URL
      }
    }).catch((e: BusinessError) => {
      hilog.error(DOMAIN, TAG, 'open WebView failed: %{public}s', e.message);
      this.helpText = '无法打开控制台页面（' + e.message + '），请确认已同步 pages/ConsoleWebPage.ets 并重新安装。\n地址：' +
        CONSOLE_URL + '\n\n' + this.helpText;
    });
  }
}
