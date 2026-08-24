import { common, Want } from '@kit.AbilityKit';
import { vpnExtension } from '@kit.NetworkKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { router } from '@kit.ArkUI';
import { OpenP2PEngine } from '../model/OpenP2PEngine';

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
  private bundleName: string = 'com.openp2p.client';
  private polling: boolean = false;
  private loggedIn: boolean = false;

  aboutToAppear(): void {
    const host = this.getUIContext().getHostContext();
    if (host) {
      const ctx = host as common.UIAbilityContext;
      this.baseDir = ctx.filesDir;
      try {
        this.bundleName = ctx.abilityInfo.bundleName;
      } catch (_e) {
        this.bundleName = 'com.openp2p.client';
      }
    }
    // 不要自动登录：否则一启动就 dlopen Go + 联网，又慢又像“已经在线且退不出”
    // 仅延迟预填 Token，避免卡住首帧
    setTimeout(() => {
      this.prefetchToken();
    }, 50);
  }

  aboutToDisappear(): void {
    this.polling = false;
  }

  private prefetchToken(): void {
    try {
      const saved = OpenP2PEngine.getToken(this.baseDir);
      if (saved && saved !== '0' && saved.length > 0) {
        this.token = saved;
      }
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'prefetchToken failed: %{public}s', JSON.stringify(e));
    }
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
          // 已登录时始终可点「退出」；未登录时需有 Token 且不在 busy
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
        .fontColor(this.onlineState === '在线' ? Color.Green : Color.Grey)
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
      if (!this.baseDir || this.baseDir.length === 0) {
        this.onlineState = '登录失败：filesDir 为空';
        return;
      }
      const native = OpenP2PEngine.nativeStatus();
      hilog.info(DOMAIN, TAG, 'before login nativeStatus=%{public}s baseDir=%{public}s bundle=%{public}s',
        native, this.baseDir, this.bundleName);
      if (native !== 'ok') {
        this.onlineState = '登录失败：native 未就绪\n' + native;
        return;
      }

      // runAsModule is a NAPI Promise — Go Connect runs off the UI thread
      const ok = await OpenP2PEngine.runAsModule(this.baseDir, this.token.trim(), 0, 1);
      if (!ok) {
        this.onlineState = '登录失败（请检查 Token / native 库）\nnativeStatus=' +
          OpenP2PEngine.nativeStatus();
        return;
      }
      this.loggedIn = true;
      this.loginLabel = '退出';
      this.onlineState = '在线';
      this.startPolling();
      // VPN 失败不能当成登录失败；不要 await 以免异常冒泡
      this.startVpnExtension();
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
    // Soft disconnect so console shows offline (not os.Exit)
    OpenP2PEngine.stop();
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
      // isOnline is non-blocking; do not call connect() on UI thread
      const online = OpenP2PEngine.isOnline();
      this.onlineState = online ? '在线' : '连接中…';
      setTimeout(tick, 3000);
    };
    setTimeout(tick, 1000);
  }

  private async startVpnExtension(): Promise<void> {
    // Official sample Want: only deviceId + bundleName + abilityName (no moduleName).
    const want: Want = {
      deviceId: '',
      bundleName: this.bundleName,
      abilityName: 'OpenP2PVpnAbility'
    };
    hilog.info(DOMAIN, TAG, 'startVpn want bundle=%{public}s ability=OpenP2PVpnAbility', this.bundleName);
    try {
      await vpnExtension.startVpnExtensionAbility(want);
      hilog.info(DOMAIN, TAG, 'VPN extension started');
      this.onlineState = '在线（VPN 已请求）';
    } catch (err) {
      const e = err as BusinessError;
      hilog.error(DOMAIN, TAG, 'startVpn failed code=%{public}d msg=%{public}s', e.code, e.message);
      const hint = (e.code === 2097152)
        ? '\n错误 2097152=系统 connectAbility 失败（Ability 未启动）。请在 PC 执行：\n' +
          'hdc shell bm dump -n ' + this.bundleName + '\n' +
          'hdc shell param get const.product.supportVpn\n' +
          '若 dump 里没有 OpenP2PVpnAbility，说明 HAP 未带上 VPN 扩展。\n' +
          '若 supportVpn 为空/0，当前镜像不支持三方 VpnExtension（DAYU200 常见）。\n'
        : '\n';
      this.onlineState = '在线（无 TUN，无法 ping 虚拟 IP）';
      this.helpText =
        '核心已登录，但 VPN 扩展失败 code=' + e.code + ' ' + e.message + hint +
        '此时控制台可显示在线，但 SD-WAN ping 不通。\n\n' + this.helpText;
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
