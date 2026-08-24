import { webview } from '@kit.ArkWeb';
import { router } from '@kit.ArkUI';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';

const DOMAIN = 0x4F50;
const TAG = 'ConsoleWeb';
const CONSOLE_URL = 'https://console.openp2p.cn/profile';

@Entry
@Component
struct ConsoleWebPage {
  private controller: webview.WebviewController = new webview.WebviewController();
  /** 普通字段即可，不要用 @State 绑 Web.src（动态改 src 无效） */
  private targetUrl: string = CONSOLE_URL;
  @State statusText: string = '正在加载控制台…';
  @State loadFailed: boolean = false;

  aboutToAppear(): void {
    const params = router.getParams() as Record<string, Object> | undefined;
    if (params !== undefined && params['url'] !== undefined) {
      const u = params['url'] as string;
      if (u.length > 0) {
        this.targetUrl = u;
      }
    }
    this.statusText = '打开：' + this.targetUrl;
  }

  private reload(): void {
    try {
      this.controller.loadUrl(this.targetUrl);
      this.loadFailed = false;
      this.statusText = '刷新：' + this.targetUrl;
    } catch (err) {
      const e = err as BusinessError;
      this.loadFailed = true;
      this.statusText = '刷新失败：' + e.message;
      hilog.error(DOMAIN, TAG, 'reload error %{public}d %{public}s', e.code, e.message);
    }
  }

  build() {
    Column() {
      Row() {
        Button('返回')
          .height(36)
          .onClick(() => {
            router.back();
          })
        Text('OpenP2P 控制台')
          .fontSize(18)
          .fontWeight(FontWeight.Medium)
          .margin({ left: 12 })
          .layoutWeight(1)
        Button('刷新')
          .height(36)
          .onClick(() => {
            this.reload();
          })
      }
      .width('100%')
      .padding({
        left: 12,
        right: 12,
        top: 8,
        bottom: 8
      })

      Text(this.statusText)
        .fontSize(12)
        .fontColor(this.loadFailed ? Color.Red : Color.Grey)
        .width('100%')
        .padding({ left: 12, right: 12, bottom: 6 })

      // 首次直接用目标 URL 作为 src，避免 about:blank + loadUrl 竞态导致必须手动刷新
      Web({ src: this.targetUrl, controller: this.controller })
        .layoutWeight(1)
        .width('100%')
        .javaScriptAccess(true)
        .domStorageAccess(true)
        .onlineImageAccess(true)
        .fileAccess(true)
        .mixedMode(MixedMode.All)
        .cacheMode(CacheMode.Default)
        .onPageBegin((event) => {
          if (event) {
            this.statusText = '打开：' + event.url;
            this.loadFailed = false;
          }
        })
        .onPageEnd((event) => {
          if (event) {
            this.statusText = '已加载：' + event.url;
          }
        })
        .onErrorReceive((_event) => {
          this.loadFailed = true;
          this.statusText = '页面加载出错，可点刷新重试';
          hilog.error(DOMAIN, TAG, 'onErrorReceive');
        })
        .onSslErrorEventReceive((event) => {
          hilog.warn(DOMAIN, TAG, 'ssl error, confirm continue');
          try {
            event.handler.handleConfirm();
          } catch (_e) {
          }
        })
    }
    .width('100%')
    .height('100%')
  }
}
