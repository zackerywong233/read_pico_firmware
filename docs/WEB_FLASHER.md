# 浏览器刷写 / Browser flashing

小纸 Pico 的浏览器安装页使用 ESP Web Tools 与 Web Serial，通过 HTTPS 连接 ESP32-S3，并写入一个 ESP-IDF 合并固件镜像。

The browser installer uses ESP Web Tools and Web Serial over HTTPS to write one merged ESP-IDF image to the ESP32-S3.

## 发布流程 / Release flow

1. 合并固件分支后，在 `main` 上创建并推送版本标签，例如 `v1.0.0`。
2. `release-firmware.yml` 使用 ESP-IDF v6.1 和 `sdkconfig.defaults` 构建产品固件，再用 `idf.py merge-bin` 生成合并镜像。
3. 工作流将镜像附加到 GitHub Release，并把同一镜像与刷写网页一起部署到 GitHub Pages。
4. 首次使用 Pages 时，需在仓库 Settings → Pages 将 Build and deployment source 设为 GitHub Actions。

After merging the firmware branch, push a version tag such as `v1.0.0` on `main`. The workflow builds with ESP-IDF v6.1 and the product `sdkconfig.defaults`, creates a merged image, attaches it to the GitHub Release, and deploys the page and image to GitHub Pages. For the first deployment, set Settings → Pages → Build and deployment source to GitHub Actions.

## 使用方式 / Install

1. 打开 [小纸 Pico 浏览器刷机页](https://zackerywong233.github.io/read_pico_firmware/)。
2. 使用最新版 Chrome 或 Edge。Firefox 与 Safari 不支持此页面所用的 Web Serial 安装流程。
3. 用 USB 数据线连接设备，点击“连接并安装”，在系统弹窗里选择 Pico 的串口。
4. 首次安装会询问是否擦除 ESP Flash。擦除会清除 ESP 内置存储和设置；TF 卡上的文件不会被擦除。
5. 等到页面显示完成后再断开 USB 并重启设备。

Open the installer in current Chrome or Edge, connect over USB, select the Pico serial port, and wait for the completion message before disconnecting. The first install asks whether to erase ESP flash. Erasing clears internal storage and settings; files on the TF card remain untouched.

## 恢复下载模式 / Enter download mode

若串口未出现，可断开 USB，按住设备 BOOT 键再重新连接，识别后松开 BOOT。刷写中断时保持电源稳定，再次进入下载模式重试。

If no serial port appears, unplug USB, hold BOOT while reconnecting, then release it after the port is detected. If flashing is interrupted, keep power stable and retry in download mode.

## 维护说明 / Maintainer notes

- `sdkconfig.ci` 仅用于编译检查。可发布固件必须从产品配置 `sdkconfig.defaults` 构建。
- ESP Web Tools 需要合并镜像：清单将它写在 Flash 偏移 `0x0`。
- 网页与固件一起部署在 GitHub Pages，清单使用同源相对路径，以满足浏览器跨域读取要求。
- 新版本标签应与仓库发布策略一致；发布前应先完成功能和真机验证。

- `sdkconfig.ci` is compile-only. Release images must use the product `sdkconfig.defaults`.
- ESP Web Tools requires a merged image; the manifest writes it at flash offset `0x0`.
- The page and image are deployed together to GitHub Pages and the manifest uses a same-origin relative URL for browser access.
- Follow the repository release policy and complete feature and device validation before creating a release tag.
