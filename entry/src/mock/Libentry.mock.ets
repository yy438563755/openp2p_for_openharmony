const NativeMock: Record<string, Object> = {
  'runAsModule': async (_baseDir: string, _token: string, _bw?: number, _logLevel?: number): Promise<boolean> => false,
  'getToken': (_baseDir: string) => '0',
  'connect': (_timeoutMs?: number) => false,
  'isOnline': () => false,
  'stop': () => {
  },
  'getNodeName': () => '',
  'getSdwanConfig': () => new ArrayBuffer(0),
  'getSdwanConfigAsync': async () => new ArrayBuffer(0),
  'ohosRead': (_pkt: ArrayBuffer | Uint8Array) => {
  },
  'ohosWrite': (_timeoutMs?: number, _maxLen?: number) => new ArrayBuffer(0),
  'startTunPump': (_tunFd: number) => false,
  'stopTunPump': () => {
  },
  'startTunBridgeHost': (_sockPath: string) => false,
  'startTunBridgeDevice': (_sockPath: string, _tunFd: number) => false,
  'stopTunBridge': () => {
  },
  'nativeStatus': () => 'mock',
  'lastError': () => '',
};

export default NativeMock;
