// ESP Web Tools 10.4.0 has no locale API and renders English strings inside
// open shadow roots. index.html pins this version so these mappings stay in sync.
const translations = new Map([
  ["Connecting", "连接中"],
  ["Initializing...", "正在初始化…"],
  ["Preparing installation...", "正在准备安装…"],
  ["Installation prepared", "安装准备完成"],
  ["Erasing device...", "正在擦除设备…"],
  ["Device erased", "设备已擦除"],
  ["Writing complete", "写入完成"],
  ["All done!", "全部完成！"],
  ["Error", "错误"],
  ["Close", "关闭"],
  ["Device Dashboard", "设备管理"],
  ["Logs & Console", "日志与串口"],
  ["Visit Device", "访问设备"],
  ["Add to Home Assistant", "添加到 Home Assistant"],
  ["Change Wi-Fi", "更换 Wi-Fi"],
  ["Connect to Wi-Fi", "连接 Wi-Fi"],
  ["Fund Development", "支持开发"],
  ["Erase User Data", "擦除用户数据"],
  ["Configure Wi-Fi", "配置 Wi-Fi"],
  ["Trying to connect", "正在连接"],
  ["Scanning for networks", "正在扫描 Wi-Fi 网络"],
  ["Device connected to the network!", "设备已连接到网络！"],
  ["Unable to connect", "无法连接"],
  ["Timeout", "连接超时"],
  ["Network", "Wi-Fi 网络"],
  ["Join other…", "手动输入其他网络…"],
  ["Network Name", "Wi-Fi 名称"],
  ["Password", "密码"],
  ["Connect", "连接"],
  ["Back", "返回"],
  ["Skip", "跳过"],
  ["Continue", "继续"],
  ["Erase device", "擦除设备"],
  ["Confirm Installation", "确认安装"],
  ["install", "安装"],
  ["update to", "更新为"],
  ["All data on the device will be erased.", "设备上的所有数据都会被擦除。"],
  ["Preparing installation", "正在准备安装"],
  ["Erasing", "正在擦除"],
  ["Installing", "正在安装"],
  ["Wrapping up", "即将完成"],
  ["This will take", "预计需要"],
  ["a minute", "约 1 分钟"],
  ["2 minutes", "约 2 分钟"],
  ["Keep this page visible to prevent slow down", "请保持此页面打开，以免刷写变慢"],
  ["Installation complete!", "安装完成！"],
  ["Next", "下一步"],
  ["Installation failed", "安装失败"],
  ["Logs", "日志"],
  ["Reset Device", "重置设备"],
  ["Download Logs", "下载日志"],
  ["Failed to download manifest", "无法下载固件清单"],
  [
    "Serial port is not readable/writable. Close any other application using it and try again.",
    "串口当前不可读写。请关闭其他正在使用串口的程序后重试。",
  ],
  [
    "Serial port is not ready. Close any other application using it and try again.",
    "串口尚未就绪。请关闭其他正在使用串口的程序后重试。",
  ],
  ["Disconnected", "设备已断开"],
  ["Do you want to erase the device before installing", "是否先擦除设备再安装"],
  ["? All data on the device will be lost.", "？设备上的所有数据都会丢失。"],
  ["Do you want to reset your device and erase all user data from your", "是否重置设备并擦除所有用户数据"],
  ["device?", "？"],
  ["Do you want to", "确定要"],
  ["Your device is running", "当前设备运行的是"],
  ["The connected device has Wi-Fi turned off, so it can't", "已连接的设备关闭了 Wi-Fi，因此暂时无法"],
  ["be configured right now.", "进行配置。"],
  ["Enable the device's Wi-Fi, then try again.", "请开启设备的 Wi-Fi 后重试。"],
  ["Connect your device to the network to start using it.", "连接设备到网络后即可开始使用。"],
]);

function translate(value) {
  const normalized = value.trim().replace(/\s+/g, " ");
  const exact = translations.get(normalized);
  if (exact) return exact;
  if (normalized === "a minute.") return "约 1 分钟。";
  if (normalized === "2 minutes.") return "约 2 分钟。";
  if (normalized.startsWith("Writing progress: ")) {
    return `写入进度：${normalized.slice(18)}`;
  }
  if (normalized.startsWith("Initialized. Found ")) {
    return `已识别设备：${normalized.slice(19)}`;
  }
  if (normalized.startsWith("Your ") && normalized.endsWith(" board is not supported.")) {
    const chip = normalized.slice(5, -" board is not supported.".length);
    return `暂不支持此 ${chip} 开发板。`;
  }
  if (normalized.startsWith("Failed to initialize.")) {
    return "设备初始化失败。请重新连接 USB 后重试；此设备没有实体 BOOT 键。";
  }
  if (normalized.startsWith("Downloading firmware ") && normalized.includes(" failed: ")) {
    return `固件下载失败：${normalized.split(" failed: ").at(-1)}`;
  }
  if (normalized.startsWith("Install ")) return `安装 ${normalized.slice(8)}`;
  if (normalized.startsWith("Update ")) return `更新 ${normalized.slice(7)}`;
  if (normalized.startsWith("Connected to ")) return `已连接：${normalized.slice(13)}`;
  if (normalized.startsWith("Unknown error (")) return `未知错误（${normalized.slice(15)}`;
  return undefined;
}

function translateRoot(root) {
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  let node;
  while ((node = walker.nextNode())) {
    const value = node.nodeValue;
    const replacement = translate(value);
    if (replacement) {
      const leading = value.match(/^\s*/)?.[0] ?? "";
      const trailing = value.match(/\s*$/)?.[0] ?? "";
      node.nodeValue = `${leading}${replacement}${trailing}`;
    }
  }

  for (const element of root.querySelectorAll("*")) {
    for (const name of ["aria-label", "label", "placeholder", "title"]) {
      const value = element.getAttribute(name);
      if (!value) continue;
      const replacement = translate(value);
      if (replacement) element.setAttribute(name, replacement);
    }
  }
}

const observedRoots = new WeakSet();
const observer = new MutationObserver((records) => {
  const roots = new Set();
  for (const record of records) {
    roots.add(record.target.getRootNode());
    for (const added of record.addedNodes) attachToSubtree(added);
  }
  for (const root of roots) {
    if (root instanceof ShadowRoot) translateRoot(root);
  }
});

function attachRoot(root) {
  if (!root || observedRoots.has(root)) return;
  observedRoots.add(root);
  observer.observe(root, {
    attributes: true,
    attributeFilter: ["aria-label", "label", "placeholder", "title"],
    childList: true,
    characterData: true,
    subtree: true,
  });
  translateRoot(root);
  for (const child of root.querySelectorAll("*")) {
    if (child.shadowRoot) attachRoot(child.shadowRoot);
  }
}

function attachToSubtree(node) {
  if (!(node instanceof Element)) return;
  if (node.shadowRoot) attachRoot(node.shadowRoot);
  for (const child of node.querySelectorAll("*")) {
    if (child.shadowRoot) attachRoot(child.shadowRoot);
  }
}

customElements.whenDefined("esp-web-install-button").then(() => {
  for (const button of document.querySelectorAll("esp-web-install-button")) {
    attachRoot(button.shadowRoot);
  }
});

observer.observe(document.body, { childList: true });
