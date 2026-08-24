import { abilityAccessCtrl, common, Permissions } from '@kit.AbilityKit';
import { notificationManager } from '@kit.NotificationKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { BusinessError } from '@kit.BasicServicesKit';

const DOMAIN = 0x4F50;
const TAG = 'AppBootstrap';

const VPN_PERMS: Array<Permissions> = [
  'ohos.permission.MANAGE_VPN',
  'ohos.permission.GET_NETWORK_INFO'
];

/** Same as VPNControl_Case Index.aboutToAppear — notification only on first launch. */
export class AppBootstrap {
  static requestNotification(context: common.UIAbilityContext): void {
    try {
      notificationManager.requestEnableNotification(context, (err: BusinessError): void => {
        if (err) {
          hilog.warn(DOMAIN, TAG, 'requestEnableNotification failed code=%{public}d', err.code);
        } else {
          hilog.info(DOMAIN, TAG, 'requestEnableNotification ok');
        }
      });
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'requestEnableNotification err: %{public}s', JSON.stringify(e));
    }
  }

  static async requestVpnPermissions(context: common.UIAbilityContext): Promise<void> {
    try {
      const atManager = abilityAccessCtrl.createAtManager();
      const result = await atManager.requestPermissionsFromUser(context, VPN_PERMS);
      hilog.info(DOMAIN, TAG, 'requestPermissionsFromUser result=%{public}s', JSON.stringify(result));
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'requestPermissionsFromUser err: %{public}s', JSON.stringify(e));
    }
  }
}
